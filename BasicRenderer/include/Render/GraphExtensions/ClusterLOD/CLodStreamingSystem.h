#pragma once

#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Managers/MeshManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodPageLRU.h"
#include "Render/GraphExtensions/ClusterLOD/CLodUploadStream.h"
#include "Resources/Buffers/Buffer.h"
#include "Resources/Resolvers/PublishedStateResourceResolver.h"
#include "Utilities/BoundedSpscQueue.h"
#include "Render/GraphExtensions/ClusterLOD/VirtualShadowUpgradeService.h"

namespace org { class UploadInstance; }
struct UpdateContext;

struct CLodActiveGroupsSnapshot {
    uint32_t activeGroupScanCount = 0;
    uint64_t generation = 0;
};

struct CLodVirtualShadowUpgradeQueueStats {
    uint64_t inputRecords = 0u;
    uint64_t uniqueInputRecords = 0u;
    uint64_t expandedDependencyPairs = 0u;
    uint64_t dependenciesObserved = 0u;
    uint64_t dependenciesDeduplicated = 0u;
    uint64_t lateResidentDependencies = 0u;
    uint64_t promotionsWithDependencies = 0u;
    uint64_t promotionsWithoutDependencies = 0u;
    uint64_t eventsQueued = 0u;
    uint64_t eventsUploaded = 0u;
    uint64_t staleEvents = 0u;
    uint64_t clearedDependencies = 0u;
    uint32_t activeDependencyGroups = 0u;
    uint32_t activeDependencyPairs = 0u;
    uint32_t queuedEvents = 0u;
    uint32_t peakActiveDependencyPairs = 0u;
    uint64_t oldestQueuedTick = 0u;
};

class CLodStreamingSystem {
public:
    CLodStreamingSystem();
    ~CLodStreamingSystem();

    // Renderer-scoped geometry storage. This dependency is installed before
    // Initialize and remains valid until Shutdown completes; streaming workers
    // no longer discover it through the process-global settings registry.
    void SetGeometryStorage(ICLodGeometryStorage* storage) noexcept { m_geometryStorage = storage; }

    void SetPriorityMode(CLodPriorityMode mode) { m_priorityMode = mode; }
    CLodPriorityMode GetPriorityMode() const { return m_priorityMode; }

    void Initialize(org::RenderGraph& rg);
    void Shutdown();
    void ShutdownGraphResources();
    void QuiesceGraphResourceAccess();
    void OnRegistryReset(org::ResourceRegistry* reg);
    void GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses);
    void GatherStructuralTailPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses);
    void GatherFramePasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses);
    std::shared_ptr<org::Buffer> GetSourceGroupMismatchCounterBuffer() const { return m_sourceGroupMismatchCounter; }
    std::shared_ptr<org::Buffer> GetSourceGroupMismatchDetailsBuffer() const { return m_sourceGroupMismatchDetails; }
    void SetVirtualShadowUpgradeUploadBuffers(std::vector<std::shared_ptr<org::Buffer>> buffers);
    VirtualShadowUpgradeQueue GetVirtualShadowUpgradeQueue() const { return m_virtualShadowUpgradeQueue; }
    void SetVirtualShadowFallbackFeedbackResources(
        std::shared_ptr<org::Buffer> dependencies,
        std::shared_ptr<org::Buffer> dependencyCount);

private:
    struct VirtualShadowDependency;
    struct VirtualShadowDependencyBucket;

    enum class CLodPhysicalPageState : uint8_t {
        Free,
        Resident,
        PreAllocatedCpuUpload,
        PendingDirectStorageWrite,
        Retiring,
    };

    enum class StreamingRequestState : uint8_t {
        None,
        PendingCpu,
        WaitingForPages,
        DiskIo,
    };

    struct StreamingServiceSummary {
        uint32_t requested = 0;
        uint32_t unique = 0;
        uint32_t applied = 0;
        uint32_t failed = 0;
    };

    struct PendingStreamingRequest {
        CLodStreamingRequest request{};
        uint32_t priority = 0u;
        uint32_t generation = 0u;
        uint64_t lastObservedTick = 0u;
    };

    struct CachedChildGroupLayout {
        uint32_t ownerGroupIndex = 0;
        CLodCache::GroupPayloadLayoutMetadata layout;
    };

    static uint32_t BitWordAddress(uint32_t key);
    static uint32_t BitMask(uint32_t key);
    static uint32_t UnpackStreamingRequestPriority(const CLodStreamingRequest& req);

    bool IsGroupPinned(uint32_t groupIndex) const;
    bool IsGroupActive(uint32_t groupIndex) const;
    bool IsGroupResident(uint32_t groupIndex) const;
    bool UsesPinnedStorage(uint32_t groupIndex) const;
    bool TryQueuePendingLoadRequest(
        const CLodStreamingRequest& req,
        uint32_t priority,
        uint64_t readbackDecodedNs);
    uint32_t QueueLoadRequestWithParents(
        const CLodStreamingRequest& requestedLoad,
        uint32_t requestedPriority,
        uint64_t readbackDecodedNs);
    void EnsureStreamingStorageCapacity(uint32_t requiredGroupCount);
    void ProcessStreamingDomainEvents();
    void RebuildStreamingDomainFromSnapshot(ICLodGeometryStorage* meshManager);
    void InitializeActiveRange(
        ICLodGeometryStorage* meshManager,
        uint32_t begin,
        uint32_t count,
        uint32_t& initializedGroups,
        uint32_t& queuedPinnedGroups);
    void ResetStreamingStateForShutdown();
    bool IsStreamingRequestInProgress(uint32_t groupIndex) const;
    void MarkStreamingRequestPending(uint32_t groupIndex);
    void MarkStreamingRequestWaitingForPages(uint32_t groupIndex);
    void MarkStreamingRequestDiskIo(uint32_t groupIndex);
    void ClearStreamingRequestInProgress(uint32_t groupIndex);
    uint32_t GetPendingLoadPriority(uint32_t groupIndex) const;
    void SetPendingLoadPriority(uint32_t groupIndex, uint32_t priority);
    void ClearPendingLoadPriority(uint32_t groupIndex);
    void PushOrUpdatePendingStreamingRequest(const CLodStreamingRequest& req, uint32_t priority);
    void ParkStreamingRequestWaitingForPages(const PendingStreamingRequest& pending);
    void RequeueWaitingForPagesRequests(uint32_t maxRequests);
    void RequeuePendingStreamingRequest(const PendingStreamingRequest& pending);
    bool PopHighestPriorityPendingStreamingRequest(PendingStreamingRequest& outRequest);
    bool RemovePendingStreamingRequestAt(
        uint32_t heapIndex,
        PendingStreamingRequest& outRequest);
    void SetGroupUsesPinnedStorage(uint32_t groupIndex, bool usesPinnedStorage);
    void ApplyDiskStreamingCompletions(ICLodGeometryStorage* meshManager);
    void ParkReadyCompletionForSharedPage(
        uint32_t groupIndex,
        uint32_t page,
        uint64_t key,
        MeshManager::CLodDiskStreamingCompletion&& completion);
    void ParkReadyCompletionForPageCredit(
        uint32_t groupIndex,
        MeshManager::CLodDiskStreamingCompletion&& completion);
    void ParkReadyCompletionForParent(
        uint32_t groupIndex,
        uint32_t parentGroupIndex,
        MeshManager::CLodDiskStreamingCompletion&& completion);
    void StoreReadyStreamingCompletion(
        uint32_t groupIndex,
        MeshManager::CLodDiskStreamingCompletion&& completion);
    void WakeReadyPageCreditWaiters(uint32_t availablePageCredits);
    void PruneStaleReadyStreamingCompletions(uint32_t maxCompletions);
    void WakeReadyCompletionsForPage(uint32_t page, uint64_t key);
    void WakeReadyCompletionsForParent(
        uint32_t parentGroupIndex,
        std::vector<MeshManager::CLodDiskStreamingCompletion>*
            immediateCompletions = nullptr);
    void CommitPendingResidencyPromotions(ICLodGeometryStorage* meshManager);
    void RecordVirtualShadowUpgradeDependencies(
        std::span<const CLodVirtualShadowPredictedPage> dependencies);
    void QueueVirtualShadowReadyDependency(const VirtualShadowDependency& dependency);
    bool InsertVirtualShadowDependency(
        VirtualShadowDependencyBucket& bucket,
        const VirtualShadowDependency& dependency);
    VirtualShadowDependencyBucket& GetOrCreateVirtualShadowDependencyBucket(
        uint32_t groupIndex);
    void RehashVirtualShadowDependencyBucket(
        VirtualShadowDependencyBucket& bucket,
        size_t capacity);
    std::vector<VirtualShadowDependency> RemoveVirtualShadowDependencyBucket(
        uint32_t groupIndex);
    void QueueVirtualShadowUpgradeForPromotion(uint32_t groupIndex);
    void PublishVirtualShadowUpgradeUpload();
    void InvalidateVirtualShadowUpgradeUploadMappings();
    void ClearVirtualShadowUpgradeState();
    void ReconcileStaleDiskIoRequests(ICLodGeometryStorage* meshManager);
    bool PromoteGroupPagesAfterUploadDrain(uint32_t groupIndex);
    void EnsureStreamingDiagnosticsCapacity(uint32_t requiredGroupCount);
    void RecordStreamingRequestObserved(
        uint32_t groupIndex,
        uint32_t priority,
        uint64_t readbackDecodedNs);
    void RecordStreamingRequestQueued(uint32_t groupIndex);
    void RecordStreamingDuplicateRequest(uint32_t groupIndex);
    void RecordStreamingDiskQueued(uint32_t groupIndex);
    void RecordStreamingCompletion(uint32_t groupIndex, const MeshManager::CLodDiskStreamingCompletion& completion);
    void RecordStreamingUploadQueued(uint32_t groupIndex, uint64_t bytes);
    void RecordStreamingCommitQueued(uint32_t groupIndex);
    void RecordStreamingUploadSubmitted(uint32_t groupIndex);
    void RecordStreamingPromoted(uint32_t groupIndex);
    void RecordStreamingTerminal(uint32_t groupIndex);
    void CompleteStreamingRequestTrace(uint32_t groupIndex, bool resident);
    void WriteStreamingRequestTraceReport();
    void AccumulateStreamingDiagnostics(CLodStreamingOperationStats& stats);
    void QueuePendingNonResidentBitsUpload();
    void CreateResidencyStorage(uint32_t capacity);
    void PublishFilledResidencyStorages(uint64_t completedBatchId);
    uint32_t BoundResidencyCapacity(const UpdateContext& context) const;
    bool IsPhysicalPageResidentForKey(uint32_t page, uint64_t key) const;
    bool IsPhysicalPagePendingForKey(uint32_t page, uint64_t key) const;
    uint32_t GetPendingMeshPageRefCount(uint32_t page, uint64_t key) const;
    void AddPendingMeshPageReference(uint32_t page, uint64_t key);
    void ReleasePendingMeshPageReference(uint32_t page, uint64_t key);
    bool SetGroupResidentBit(uint32_t groupIndex, bool resident);
    void ForceGroupNonResident(uint32_t groupIndex, ICLodGeometryStorage* meshManager, bool clearPageMapEntries);
    void ForceGroupAndDescendantsNonResident(
        uint32_t groupIndex,
        ICLodGeometryStorage* meshManager,
        bool clearPageMapEntries);
    bool IsGroupSelectedParentResident(uint32_t groupIndex, ICLodGeometryStorage* meshManager) const;
    bool IsGroupSelectedParentResidentOrCommitReady(
        uint32_t groupIndex,
        ICLodGeometryStorage* meshManager) const;
    uint32_t SelectedAncestorDepth(
        uint32_t groupIndex,
        ICLodGeometryStorage* meshManager) const;
    void TouchGroupPages(uint32_t groupIndex);
    void PrefetchChildGroupLayouts(uint32_t parentGroupIndex, ICLodGeometryStorage* meshManager);
    void InstallPrefetchedChildGroupLayouts(
        uint32_t parentGroupIndex,
        std::vector<MeshManager::CLodPrefetchedChildLayout>&& prefetchedLayouts);
    void EvictPrefetchedChildLayoutsForOwner(uint32_t ownerGroupIndex);
    void ClearPrefetchedChildLayouts();
    void PollCompletedReadbackSlots();
    void StreamingDrainTask(const br::TaskContext& context);
    void ScheduleStreamingDrain();
    void ProcessStreamingRequestsBudgeted();
    void RequestStreamingFrameWork();
    void PublishStreamingFrameWorkForFrame();
    void RunStreamingServiceWork();
    bool EnsureParallelSortResources();
    void DestroyParallelSortResources();
    void ClearStreamingUploadFunction(ICLodGeometryStorage* meshManager);
    void InstallStreamingUploadFunction(ICLodGeometryStorage* meshManager);
    bool PublishRetainedUploadBatch();
    void SealStreamingUploadBatch();
    void ObserveUploadBatchTickets();
    void PublishActiveGroupSnapshot();
    void StartStreamingService();
    void StopStreamingService();

    // Page-level LRU helpers
    void InitializePageLru(ICLodGeometryStorage* meshManager);
    void EnsurePageTrackingCapacity(ICLodGeometryStorage* meshManager);
    struct PagePopFailureStats {
        uint32_t scanned = 0;
        uint32_t scanLimit = 0;
        uint32_t evictionBudgetLimit = 0;
        uint32_t evictionsUsed = 0;
        uint32_t rejectedUncommittedRef = 0;
        uint32_t rejectedProtected = 0;
        uint32_t rejectedPendingWrite = 0;
        uint32_t rejectedHierarchy = 0;
        uint32_t rejectedEvictFailed = 0;
        uint32_t rejectedEvictionBudget = 0;
        uint32_t rejectedDirtyMetadata = 0;
        uint32_t evicted = 0;
        uint32_t freeClean = 0;
    };
    std::vector<uint32_t> PopFreePages(
        std::span<const uint32_t> pageSizeBytes,
        ICLodGeometryStorage* meshManager,
        PagePopFailureStats* outStats = nullptr);
    CLodPageLRU& PageLruForPage(uint32_t page);
    const CLodPageLRU& PageLruForPage(uint32_t page) const;
    uint32_t TotalPageLruSize() const;
    void ReleaseOwnedPagesForGroup(uint32_t groupIndex, ICLodGeometryStorage* meshManager);
    void ReleaseGroupResidency(uint32_t groupIndex, ICLodGeometryStorage* meshManager, bool clearPageMapEntries);
    void RetirePhysicalPage(uint32_t page, ICLodGeometryStorage* meshManager, bool pinned);
    void DrainRetiredPhysicalPages(ICLodGeometryStorage* meshManager);
    bool IsPhysicalPageRetired(uint32_t page);
    bool IsPhysicalPagePinnedStorage(uint32_t page) const;
    uint64_t StreamingUploadVisibilityDelayTicks() const;
    void RecordNonResidentBitsUploadQueued();
    void LogPageOverwriteInvariant(
        uint32_t page,
        uint32_t newGroupIndex,
        uint32_t segmentIndex,
        uint64_t meshPageKey,
        const char* reason) const;
    bool DoesGroupReferencePhysicalPage(uint32_t groupIndex, uint32_t page) const;
    bool DoesGroupReferencePageKey(uint32_t groupIndex, uint32_t page, uint64_t key) const;
    uint32_t CountResidentGroupsForPageKey(uint32_t page, uint64_t key) const;
    uint32_t FindResidentGroupForPageKey(uint32_t page, uint64_t key) const;
    uint32_t ScrubStaleResidentGroups(uint32_t page);
    void ProtectGroupAndAncestors(uint32_t groupIndex);
    void BeginPageProtectionUpdate();
    bool MarkGroupProtectedThisUpdate(uint32_t groupIndex);
    void MarkPageProtectedThisUpdate(uint32_t page);
    bool TryGetCachedParentGroup(uint32_t groupIndex, uint32_t& outParentGroupIndex);
    bool IsPhysicalPageCleanForFreshAllocation(uint32_t page) const;
    bool IsPhysicalPageEvictable(uint32_t page) const;
    bool EvictPhysicalPage(uint32_t page, ICLodGeometryStorage* meshManager);
    void MarkStreamingNonResidentBitsDirtyWord(uint32_t wordAddress);
    void MarkStreamingNonResidentBitsDirtyAll();
    bool TryConsumeStreamingNonResidentBitsUpload(std::vector<uint32_t>& outBits, uint32_t& outFirstWord, uint32_t maxWords);
    void MarkStreamingActiveGroupsBitsDirty();

    struct PreAllocatedPages {
		std::vector<uint32_t> pagesBySegment; // segment index to page ID
        std::vector<bool> segmentNeedsFetch;  // true = need disk data; false = reused still-valid page
        std::vector<uint64_t> meshPageKeys;    // physical page identity key for each page slot
        uint32_t requestGeneration = 0u;
        uint32_t segmentCount = 0;
        bool usesPinnedStorage = false;
    };

    struct CommittedGroupPageMap {
        std::vector<PagePool::PageAllocation> pageAllocations;
        std::vector<GroupPageMapEntry> pageMapEntries;
        uint64_t commitTick = 0u;
    };

    PreAllocatedPages PreAllocatePagesForGroup(uint32_t groupIndex, const MeshManager::CLodGroupStreamingInfo& info, ICLodGeometryStorage* meshManager);
    PreAllocatedPages PreAllocatePagesForGroup(
        uint32_t groupIndex,
        uint32_t groupsBase,
        std::span<const uint32_t> meshPageIndices,
        std::span<const uint32_t> meshPageBlobSizes,
        ICLodGeometryStorage* meshManager,
        bool buildMeshPageKeys = true);
    bool AssignPagesToGroup(uint32_t groupIndex, const PreAllocatedPages& pages, ICLodGeometryStorage* meshManager);
    void ReleasePreAllocatedPages(const PreAllocatedPages& pages, ICLodGeometryStorage* meshManager);
    bool ValidateRenderableCompletion(
        uint32_t groupIndex,
        const PreAllocatedPages& pages,
        const MeshManager::CLodDiskStreamingCompletion& completion,
        uint32_t expectedPageCount,
        ICLodGeometryStorage* meshManager) const;

    // Non-resident bitset allocations, newest last. A capacity increase is a new
    // allocation that the worker fills through its upload batches and publishes
    // as a CLodResidencyStorage revision once that fill completes; it never
    // resizes a bitset a frame may be reading. Every allocation a published
    // state can still bind keeps receiving residency changes in the same batches
    // as the newest, so page reuse remains ordered after every reader.
    struct ResidencyStorage {
        uint32_t capacity = 0;
        std::shared_ptr<org::Buffer> retained; // until published
        std::weak_ptr<org::Buffer> buffer;
        bool fillQueued = false;
        uint64_t fillBatchId = 0;
        bool published = false;
    };
    std::vector<ResidencyStorage> m_residencyStorages; // streaming worker
    // Frames bind the bitset of their published geometry cut; before the first
    // cut this falls back to the initial allocation.
    std::shared_ptr<PublishedStateResourceResolver> m_nonResidentBitsResolver;
    std::shared_ptr<org::Buffer> m_initialResidencyStorage; // resolver fallback
    std::shared_ptr<org::Buffer> m_streamingLoadRequestKeys;
    std::shared_ptr<org::Buffer> m_streamingLoadRequests;
    std::shared_ptr<org::Buffer> m_streamingLoadCounter;
    std::shared_ptr<org::Buffer> m_streamingRuntimeState;
    std::shared_ptr<org::Buffer> m_usedGroupsCounter;
    std::shared_ptr<org::Buffer> m_usedGroupsBuffer;
    std::shared_ptr<org::Buffer> m_sourceGroupMismatchCounter;
    std::shared_ptr<org::Buffer> m_sourceGroupMismatchDetails;

    std::vector<uint32_t> m_streamingNonResidentBitsCpu;
    std::vector<uint32_t> m_streamingActiveGroupsBitsCpu;
    std::vector<uint32_t> m_streamingPinnedGroupsBitsCpu;
    std::vector<uint32_t> m_streamingResidencyInitializedBitsCpu;
    std::vector<uint32_t> m_usedGroupsBitsCpu; // groups reported as visible by the GPU last frame
    std::vector<uint32_t> m_usedGroupsWordsCpu;
    std::vector<uint64_t> m_groupLastUsedTick;
    std::vector<uint32_t> m_recentlyUsedGroupsCpu;
    std::vector<uint8_t> m_recentlyUsedGroupTrackedCpu;
    std::vector<uint32_t> m_parentGroupByGroup;
    std::unordered_map<uint32_t, CachedChildGroupLayout> m_prefetchedChildLayoutsByGroup;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_prefetchedChildLayoutKeysByOwner;
    std::unordered_set<uint32_t> m_errorOverriddenGroups; // groups whose GPU error is currently 0
    std::array<CLodPageLRU, PagePool::GetPageSizeClassCount()> m_pageLrus;
	std::vector<int32_t> m_pageOwnerGroup;       // page ID to group global index (-1 = unowned)
	std::vector<uint32_t> m_pageOwnerSegment;    // page ID to segment index within owning group
    std::vector<CLodPhysicalPageState> m_pageState;
    std::vector<uint64_t> m_pageRetireAfterTick;
    std::vector<uint8_t> m_pageRetirePinned;
    std::vector<uint8_t> m_pagePinnedStorage;
    std::vector<uint64_t> m_pageReuseRequiresNonResidentEpoch;
    std::vector<uint64_t> m_pageReuseNonResidentQueuedTick;
    std::vector<uint64_t> m_pageReuseUploadFenceValue;
    std::vector<uint32_t> m_retiringPhysicalPages;
    std::vector<uint32_t> m_retiringPagesAwaitingUploadFence;
    std::vector<uint32_t> m_pendingPageOwnerGroup;
    std::vector<uint32_t> m_pendingPageOwnerSegment;
    std::vector<uint64_t> m_pageOwnerMeshPageKey;
    std::vector<std::unordered_set<uint32_t>> m_pageResidentGroups;
    std::vector<uint8_t> m_pageProtectedThisUpdate;
    std::vector<uint32_t> m_pagesProtectedThisUpdate;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_groupOwnedPages; // group to page IDs by segment (~0u = no page)
    std::unordered_map<uint32_t, std::vector<uint64_t>> m_groupOwnedMeshPageKeys; // group to mesh-page keys by page slot
    std::unordered_map<uint32_t, CommittedGroupPageMap> m_groupCommittedPageMaps;
    std::unordered_map<uint64_t, uint32_t> m_residentMeshPageToPhysicalPage;
    std::unordered_map<uint64_t, uint32_t> m_residentMeshPageRefCounts;
    std::unordered_map<uint64_t, uint32_t> m_pendingMeshPageToPhysicalPage;
    std::unordered_map<uint64_t, uint32_t> m_pendingMeshPageRefCounts;
    std::unordered_map<uint32_t, PreAllocatedPages> m_preAllocatedPagesByGroup;
    std::unordered_map<uint32_t, MeshManager::CLodDiskStreamingCompletion> m_readyStreamingCompletionsByGroup;
    std::vector<uint32_t> m_readyStreamingCompletionRetryGroups;
    std::vector<uint8_t> m_readyStreamingCompletionRetryQueuedByGroup;
    std::vector<uint32_t> m_readyStreamingCompletionPageCreditWaitGroups;
    std::vector<uint8_t>
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup;
    size_t m_readyStreamingCompletionPageCreditWaitCursor = 0u;
    uint64_t m_readyStreamingCompletionBytes = 0u;
    uint64_t m_peakReadyStreamingCompletionBytes = 0u;
    uint32_t m_peakReadyStreamingCompletionCount = 0u;
    uint64_t m_transactionalChildCompletionAdmissions = 0u;
    uint64_t m_transactionalChildPromotions = 0u;
    std::vector<uint32_t> m_staleReadyCompletionGroupsScratch;
    std::vector<uint32_t> m_readyStreamingCompletionWaitPageByGroup;
    std::vector<uint64_t> m_readyStreamingCompletionWaitKeyByGroup;
    std::vector<uint32_t> m_readyStreamingCompletionWaitGenerationByGroup;
    std::vector<std::vector<uint32_t>> m_readyStreamingCompletionWaitersByPage;
    std::vector<uint32_t> m_readyStreamingCompletionWaitParentByGroup;
    std::vector<uint32_t> m_readyStreamingCompletionWaitParentGenerationByGroup;
    std::unordered_map<uint32_t, std::vector<uint32_t>>
        m_readyStreamingCompletionWaitersByParent;
    std::unordered_set<uint32_t> m_pendingResidencyCommitGroups;
    std::unordered_map<uint32_t, uint64_t> m_pendingResidencyUploadFenceByGroup;
    std::vector<uint32_t> m_residencyGroupsAwaitingUploadFence;
    std::vector<StreamingRequestState> m_streamingRequestStateByGroup;
    std::vector<uint32_t> m_pendingLoadPriorityByGroup;
    std::vector<PendingStreamingRequest> m_waitingForPagesRequests;
    std::vector<uint32_t> m_waitingForPagesRequestIndexByGroup;
    uint32_t m_waitingForPagesRequestCount = 0u;

    struct StreamingDiagnosticsRecord {
        uint64_t requestId = 0u;
        uint64_t readbackDecodedNs = 0u;
        uint64_t cpuQueuedNs = 0u;
        uint64_t diskQueuedNs = 0u;
        uint64_t ioTaskQueuedNs = 0u;
        uint64_t ioTaskStartedNs = 0u;
        uint64_t ioTaskCompletedNs = 0u;
        uint64_t diskCompletedNs = 0u;
        uint64_t uploadQueuedNs = 0u;
        uint64_t commitQueuedNs = 0u;
        uint64_t uploadSubmittedNs = 0u;
        uint64_t residentNs = 0u;
        uint64_t firstRequestTick = 0u;
        uint64_t cpuQueuedTick = 0u;
        uint64_t diskQueuedTick = 0u;
        uint64_t diskCompletedTick = 0u;
        uint64_t uploadQueuedTick = 0u;
        uint64_t commitQueuedTick = 0u;
        uint64_t residentTick = 0u;
        uint64_t lastRequestTick = 0u;
        uint64_t uploadedBytes = 0u;
        uint32_t priority = 0u;
        uint32_t duplicateRequests = 0u;
        uint32_t preallocationDeferrals = 0u;
        uint32_t promotionDeferrals = 0u;
        bool liveAtAdmission = false;
        bool active = false;
    };
    struct CompletedStreamingRequestTrace {
        uint32_t groupIndex = UINT32_MAX;
        bool resident = false;
        StreamingDiagnosticsRecord diagnostics{};
    };
    std::vector<StreamingDiagnosticsRecord> m_streamingDiagnosticsByGroup;
    std::vector<CompletedStreamingRequestTrace>
        m_completedStreamingRequestTraces;
    uint64_t m_nextStreamingRequestTraceId = 1u;
    uint64_t m_droppedStreamingRequestTraceCount = 0u;
    uint32_t m_streamingDiagnosticsDecodedRequestsThisFrame = 0u;
    uint32_t m_streamingDiagnosticsQueuedLoadRequestsThisFrame = 0u;
    uint32_t m_streamingDiagnosticsDuplicateRequestsThisFrame = 0u;
    uint32_t m_streamingDiagnosticsPreallocationDeferralsThisFrame = 0u;
    uint32_t m_streamingDiagnosticsPromotionDeferralsThisFrame = 0u;
    uint32_t m_streamingDiagnosticsCompletionSuccessThisFrame = 0u;
    uint32_t m_streamingDiagnosticsCompletionFailedThisFrame = 0u;
    uint32_t m_streamingDiagnosticsUploadQueuedGroupsThisFrame = 0u;
    uint64_t m_streamingDiagnosticsUploadQueuedBytesThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToUploadSamplesThisFrame = 0u;
    uint64_t m_streamingDiagnosticsRequestToUploadSumThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToUploadWorstThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToResidentSamplesThisFrame = 0u;
    uint64_t m_streamingDiagnosticsRequestToResidentSumThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToResidentWorstThisFrame = 0u;
    uint32_t m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame = 0u;
    uint32_t m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame = 0u;
    uint64_t m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame = 0u;
    uint32_t m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame = 0u;
    uint32_t m_streamingDiagnosticsUploadToResidentSamplesThisFrame = 0u;
    uint64_t m_streamingDiagnosticsUploadToResidentSumThisFrame = 0u;
    uint32_t m_streamingDiagnosticsUploadToResidentWorstThisFrame = 0u;
    uint32_t m_streamingDiagnosticsCommitToResidentSamplesThisFrame = 0u;
    uint64_t m_streamingDiagnosticsCommitToResidentSumThisFrame = 0u;
    uint32_t m_streamingDiagnosticsCommitToResidentWorstThisFrame = 0u;
    uint64_t m_streamingDiagnosticsLastOutlierLogTick = 0u;
    uint32_t m_streamingRequestsInProgressCount = 0u;
    uint32_t m_pendingStreamingRequestCount = 0u;
    uint32_t m_pagePopEvictionsThisUpdate = 0u;
    uint32_t m_pagePopEvictionBudgetThisUpdate = 0u;
    std::unordered_set<uint32_t> m_groupsUsingPinnedStorage;
    bool m_pageLruInitialized = false;
    uint32_t m_streamingResidentGroupsCount = 0u;
    uint32_t m_streamingActiveGroupScanCount = 0u;
    uint32_t m_streamingStorageGroupCapacity = CLodStreamingInitialGroupCapacity;
    std::atomic<uint32_t> m_streamingGpuStorageGroupCapacity{CLodStreamingInitialGroupCapacity};
    bool m_streamingNonResidentBitsUploadPending = false;
    bool m_streamingActiveGroupsBitsUploadPending = true;
    uint32_t m_streamingNonResidentBitsDirtyBegin = 0u;
    uint32_t m_streamingNonResidentBitsDirtyEnd = 0u;
    std::vector<uint32_t> m_streamingNonResidentBitsDirtyWords;
    std::vector<uint8_t> m_streamingNonResidentBitsDirtyWordFlags;
    size_t m_streamingNonResidentBitsDirtyWordCursor = 0u;
    bool m_streamingNonResidentBitsDirtyWordsSorted = true;
    uint32_t m_streamingReadbackRingSize = 3u;
    uint32_t m_streamingCpuUploadBudgetRequests = 0u;
    uint64_t m_prevTotalStreamedBytes = 0u;
    uint64_t m_streamingResidencyMutationEpoch = 0u;
    uint64_t m_streamingNonResidentBitsQueuedEpoch = 0u;
    uint64_t m_streamingNonResidentBitsQueuedTick = 0u;
    uint64_t m_streamingNonResidentBitsUploadFenceEpoch = 0u;
    uint64_t m_streamingNonResidentBitsUploadFenceValue = 0u;
    ICLodGeometryStorage* m_geometryStorage = nullptr;
    std::function<uint32_t()> m_getStreamingCpuUploadBudgetRequests;

    std::vector<PendingStreamingRequest> m_pendingStreamingRequests;
    std::vector<uint32_t> m_pendingStreamingRequestHeapIndexByGroup;
    std::vector<uint32_t> m_pendingStreamingRequestGenerationByGroup;
    CLodPriorityMode m_priorityMode = CLodPriorityMode::Max;
    uint64_t m_streamingDiagnosticTick = 0;
    uint32_t m_streamingIoAdmissionDepth = 0u;
    uint32_t m_streamingIoWorkerCount = 0u;
    uint32_t m_streamingIoTaskBatchSize = 0u;

    struct VirtualShadowPageKey {
        uint32_t physicalPageIndex = 0u;
        uint32_t allocationGeneration = 0u;
        uint32_t contentGeneration = 0u;
        bool operator==(const VirtualShadowPageKey&) const = default;
    };
    struct VirtualShadowDependency {
        CLodVirtualShadowPageToken page{};
        uint32_t residencyGeneration = 0u;
    };
    struct VirtualShadowDependencyBucket {
        uint32_t groupIndex = UINT32_MAX;
        uint32_t dependencyCount = 0u;
        std::vector<VirtualShadowDependency> dependencies;
    };
    struct VirtualShadowMissingGroup {
        uint32_t groupIndex = UINT32_MAX;
        uint32_t residencyGeneration = 0u;
        uint32_t dependencyBucketIndex = UINT32_MAX;
    };
    enum class VirtualShadowUpgradeUploadState : uint8_t {
        Free,
        Filling,
        Published,
    };
    struct VirtualShadowUpgradeUploadSlot {
        std::shared_ptr<org::Buffer> buffer;
        void* mapped = nullptr;
        uint64_t mappedBackingGeneration = 0u;
        std::atomic<VirtualShadowUpgradeUploadState> state{
            VirtualShadowUpgradeUploadState::Free};
        std::atomic<uint32_t> inputCount{0u};
    };
    static constexpr uint32_t VirtualShadowUpgradeUploadSlotCapacity = 8u;
    std::vector<VirtualShadowDependencyBucket> m_virtualShadowDependencyBuckets;
    std::vector<int32_t> m_virtualShadowDependencyBucketIndexByGroup;
    uint64_t m_virtualShadowActiveDependencyPairCount = 0u;
    uint64_t m_virtualShadowActiveDependencySlotCount = 0u;
    std::vector<std::vector<VirtualShadowDependency>> m_virtualShadowDependencyBucketPool;
    std::vector<VirtualShadowDependency> m_virtualShadowReadyByPhysicalPage;
    std::vector<uint8_t> m_virtualShadowReadyFlagsByPhysicalPage;
    std::vector<uint32_t> m_virtualShadowReadyTouchedPhysicalPages;
    std::vector<CLodVirtualShadowPredictedPage> m_virtualShadowReadbackBatchScratch;
    std::vector<VirtualShadowMissingGroup> m_virtualShadowMissingGroupsScratch;
    std::vector<uint32_t> m_virtualShadowBatchSourceGenerationByGroup;
    std::vector<uint32_t> m_virtualShadowBatchSourceChainOffsetByGroup;
    std::vector<uint32_t> m_virtualShadowBatchSourceChainCountByGroup;
    uint32_t m_virtualShadowBatchSourceGeneration = 0u;
    std::array<std::shared_ptr<VirtualShadowUpgradeUploadSlot>, VirtualShadowUpgradeUploadSlotCapacity>
        m_virtualShadowUpgradeUploadSlots;
    uint32_t m_virtualShadowUpgradeUploadSlotCount = 0u;
    VirtualShadowUpgradeQueue m_virtualShadowUpgradeQueue;
    std::vector<uint32_t> m_virtualShadowResidencyGenerationByGroup;
    CLodVirtualShadowUpgradeQueueStats m_virtualShadowUpgradeStats;
    std::shared_ptr<org::Buffer> m_virtualShadowFallbackDependenciesBuffer;
    std::shared_ptr<org::Buffer> m_virtualShadowFallbackDependencyCountBuffer;

    std::vector<MeshManager::CLodStreamingDomainEvent> m_streamingDomainEventScratch;
    std::vector<uint32_t> m_childGroupsScratch;
    uint64_t m_lastStreamingDomainEventGeneration = 0;
    // A newly-created streaming owner has not consumed the already-live mesh
    // domain. Incremental domain events are not replayable, so its first update
    // must bootstrap residency from MeshManager's authoritative snapshot.
    bool m_streamingDomainFullResetPending = true;

    struct StreamingWakeState {
        std::mutex mutex;
        CLodStreamingSystem* owner = nullptr;
    };
    std::shared_ptr<StreamingWakeState> m_streamingWakeState =
        std::make_shared<StreamingWakeState>();
    std::atomic<uint64_t> m_streamingServiceEpoch{1};
    std::atomic<bool> m_streamingServiceRunning{false};
    uint64_t m_streamingServicePublishedGeneration = 0;
    uint32_t m_publishedActiveGroupScanCount = 0;
    BoundedSpscQueue<CLodActiveGroupsSnapshot, 4> m_activeGroupsSnapshotQueue;
    std::optional<CLodActiveGroupsSnapshot> m_retainedActiveGroupsSnapshot;

    BoundedSpscQueue<std::shared_ptr<CLodUploadBatch>, 16> m_uploadBatchQueue;
    std::shared_ptr<CLodUploadBatch> m_retainedUploadBatch;
    std::vector<std::shared_ptr<CLodUploadBatch>> m_outstandingUploadBatches;
    std::atomic<uint64_t> m_nextUploadBatchId{0};
    uint64_t m_uploadBatchGeneration = 1;
    uint64_t m_cancelledUploadBatchCount = 0;
    uint64_t m_replayedUploadBatchCount = 0;

    // Self-managed readback pipeline 
    // Dedicated fence signalled when a readback copy completes on the copy queue.
    rhi::TimelinePtr m_streamingReadbackFencePtr;
    rhi::Timeline m_streamingReadbackFenceHandle;
    std::atomic<uint64_t> m_streamingReadbackFenceCounter{0};
    std::atomic<uint64_t> m_streamingReadbackDiscardedFenceCounter{0};
    rhi::TimelinePtr m_streamingUploadCompletionFencePtr;
    rhi::Timeline m_streamingUploadCompletionFenceHandle;
    std::atomic<uint64_t> m_streamingUploadCompletionFenceCounter{0};
    std::shared_ptr<rhi::TimelinePtr> m_directStorageLaunchFencePtr;
    rhi::Timeline m_directStorageLaunchFenceHandle;
    std::atomic<uint64_t> m_directStorageLaunchFenceCounter{0};
    // Worker publishes launch demand; the graph thread supplies the queue
    // fence, and the worker consumes it after completion.
    std::atomic<bool> m_directStorageLaunchRequested{false};
    std::atomic<uint64_t> m_directStorageArmedLaunchFenceValue{0};

    struct ReadbackStagingSlot {
        enum class State : uint8_t { Free, Recording, Submitted, Decoding };
        std::shared_ptr<org::Buffer> counterStaging;
        std::shared_ptr<org::Buffer> requestsStaging;
        std::shared_ptr<org::Buffer> usedGroupsCounterStaging;
        std::shared_ptr<org::Buffer> usedGroupsBufferStaging;
        std::shared_ptr<org::Buffer> sourceGroupMismatchCounterStaging;
        std::shared_ptr<org::Buffer> sourceGroupMismatchDetailsStaging;
        std::shared_ptr<org::Buffer> virtualShadowDependencyCountStaging;
        std::shared_ptr<org::Buffer> virtualShadowDependenciesStaging;
        uint64_t fenceValue = 0;
        std::atomic<State> state{State::Free};

        ReadbackStagingSlot() = default;
        ReadbackStagingSlot(const ReadbackStagingSlot&) = delete;
        ReadbackStagingSlot& operator=(const ReadbackStagingSlot&) = delete;
        ReadbackStagingSlot(ReadbackStagingSlot&& other) noexcept
            : counterStaging(std::move(other.counterStaging))
            , requestsStaging(std::move(other.requestsStaging))
            , usedGroupsCounterStaging(std::move(other.usedGroupsCounterStaging))
            , usedGroupsBufferStaging(std::move(other.usedGroupsBufferStaging))
            , sourceGroupMismatchCounterStaging(std::move(other.sourceGroupMismatchCounterStaging))
            , sourceGroupMismatchDetailsStaging(std::move(other.sourceGroupMismatchDetailsStaging))
            , virtualShadowDependencyCountStaging(std::move(other.virtualShadowDependencyCountStaging))
            , virtualShadowDependenciesStaging(std::move(other.virtualShadowDependenciesStaging))
            , fenceValue(other.fenceValue)
            , state(other.state.load(std::memory_order_relaxed)) {}
        ReadbackStagingSlot& operator=(ReadbackStagingSlot&& other) noexcept {
            counterStaging = std::move(other.counterStaging);
            requestsStaging = std::move(other.requestsStaging);
            usedGroupsCounterStaging = std::move(other.usedGroupsCounterStaging);
            usedGroupsBufferStaging = std::move(other.usedGroupsBufferStaging);
            sourceGroupMismatchCounterStaging = std::move(other.sourceGroupMismatchCounterStaging);
            sourceGroupMismatchDetailsStaging = std::move(other.sourceGroupMismatchDetailsStaging);
            virtualShadowDependencyCountStaging = std::move(other.virtualShadowDependencyCountStaging);
            virtualShadowDependenciesStaging = std::move(other.virtualShadowDependenciesStaging);
            fenceValue = other.fenceValue;
            state.store(other.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };
    std::vector<ReadbackStagingSlot> m_readbackStagingSlots;
    uint32_t m_readbackStagingCursor = 0;
    uint64_t m_readbackSlotFullEvents = 0;
    bool m_virtualShadowFeedbackLossPending = false;
    uint64_t m_virtualShadowFeedbackRecoveryRequests = 0u;

    // Serial scheduler-owned streaming coordinator. GPU completion is polled
    // by frame/service kicks; this task never waits on a fence.
    br::TaskScope m_streamingTaskScope;
    std::atomic<bool> m_streamingDrainScheduled{false};
    std::atomic<bool> m_streamingServiceStop{false};
    uint64_t m_streamingLastProcessedFence = 0;
    uint64_t m_streamingObservedServiceEpoch = 0;
    uint64_t m_streamingLastLongSliceDiagnosticMs = 0;
    struct DecodedStreamingRequest {
        uint32_t groupIndex = UINT32_MAX;
        uint32_t priority = 0u;
        uint64_t decodedNs = 0u;
    };
    // Decoded requests produced by the worker, consumed by the streaming service.
    std::vector<DecodedStreamingRequest> m_decodedReadbackBatch;
    // Deduplicated group indices from the GPU used-groups buffer, consumed by
    // the streaming service for page protection and LRU bookkeeping.
    std::vector<uint32_t> m_decodedUsedGroupsBatch;
    uint64_t m_decodedUsedGroupsSampleGeneration = 0;
    uint64_t m_usedGroupsCpuSampleGeneration = 0;
    std::vector<DecodedStreamingRequest> m_readbackBatchScratch;
    std::vector<uint32_t> m_usedGroupsBatchScratch;
    std::vector<uint32_t> m_expiredReadbackGapGroupsScratch;
    std::vector<uint32_t> m_parentChainScratch;
    std::vector<uint32_t> m_protectedGroupsBitsScratch;
    std::vector<uint32_t> m_protectedGroupWordsScratch;
    std::vector<uint32_t> m_decodeSeenGenerationByGroup;
    std::vector<uint32_t> m_decodePriorityAccumByGroup;
    std::vector<uint64_t> m_decodeFirstSeenNsByGroup;
    std::vector<uint32_t> m_decodeUsedSeenGenerationByGroup;
    uint32_t m_decodeSeenGeneration = 1u;
    uint32_t m_decodeUsedSeenGeneration = 1u;

    struct ParallelSortState;
    std::unique_ptr<ParallelSortState> m_parallelSortState;
    bool m_parallelSortAvailable = false;
    bool m_parallelSortAttempted = false;

    // Dedicated upload instance + copy queue for async CLod streaming uploads.
    std::unique_ptr<CLodUploadStream> m_uploadStream;
    org::QueueSlotIndex m_uploadQueueSlot{};
};
