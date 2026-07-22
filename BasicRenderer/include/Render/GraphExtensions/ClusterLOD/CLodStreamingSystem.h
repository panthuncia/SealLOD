#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Managers/MeshManager.h"
#include "Render/RenderGraph/RenderGraph.h"
#include "Render/GraphExtensions/CLodTelemetry.h"
#include "Render/GraphExtensions/ClusterLOD/CLodCommon.h"
#include "Render/GraphExtensions/ClusterLOD/CLodPageLRU.h"
#include "Render/GraphExtensions/ClusterLOD/CLodUploadStream.h"
#include "Resources/Buffers/Buffer.h"
#include "Utilities/BoundedSpscQueue.h"

class UploadInstance;

struct CLodActiveGroupsSnapshot {
    std::vector<uint32_t> bits;
    uint32_t activeGroupScanCount = 0;
    uint64_t generation = 0;
};

class CLodStreamingSystem {
public:
    CLodStreamingSystem();
    ~CLodStreamingSystem();

    void SetPriorityMode(CLodPriorityMode mode) { m_priorityMode = mode; }
    CLodPriorityMode GetPriorityMode() const { return m_priorityMode; }

    void Initialize(RenderGraph& rg);
    void Shutdown();
    void ShutdownGraphResources();
    void OnRegistryReset(ResourceRegistry* reg);
    void GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    void GatherStructuralTailPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    void GatherFramePasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses);
    std::shared_ptr<Buffer> GetSourceGroupMismatchCounterBuffer() const { return m_sourceGroupMismatchCounter; }
    std::shared_ptr<Buffer> GetSourceGroupMismatchDetailsBuffer() const { return m_sourceGroupMismatchDetails; }

private:
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
    bool TryQueuePendingLoadRequest(const CLodStreamingRequest& req, uint32_t priority);
    uint32_t QueueLoadRequestWithParents(const CLodStreamingRequest& requestedLoad, uint32_t requestedPriority);
    void EnsureStreamingStorageCapacity(uint32_t requiredGroupCount);
    void ProcessStreamingDomainEvents();
    void RebuildStreamingDomainFromSnapshot(MeshManager* meshManager);
    void InitializeActiveRange(
        MeshManager* meshManager,
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
    void SetGroupUsesPinnedStorage(uint32_t groupIndex, bool usesPinnedStorage);
    void ApplyDiskStreamingCompletions(MeshManager* meshManager);
    void CommitPendingResidencyPromotions();
    void ReconcileStaleDiskIoRequests(MeshManager* meshManager);
    bool PromoteGroupPagesAfterUploadDrain(uint32_t groupIndex);
    void EnsureStreamingDiagnosticsCapacity(uint32_t requiredGroupCount);
    void RecordStreamingRequestObserved(uint32_t groupIndex, uint32_t priority);
    void RecordStreamingRequestQueued(uint32_t groupIndex);
    void RecordStreamingDuplicateRequest(uint32_t groupIndex);
    void RecordStreamingDiskQueued(uint32_t groupIndex);
    void RecordStreamingCompletion(uint32_t groupIndex, const MeshManager::CLodDiskStreamingCompletion& completion);
    void RecordStreamingUploadQueued(uint32_t groupIndex, uint64_t bytes);
    void RecordStreamingCommitQueued(uint32_t groupIndex);
    void RecordStreamingPromoted(uint32_t groupIndex);
    void RecordStreamingTerminal(uint32_t groupIndex);
    void AccumulateStreamingDiagnostics(CLodStreamingOperationStats& stats);
    void QueuePendingNonResidentBitsUpload();
    void RequestStreamingStorageGpuResize(uint32_t newCapacity);
    bool PublishPendingStreamingStorageGpuResizeLocked();
    bool IsPhysicalPageResidentForKey(uint32_t page, uint64_t key) const;
    bool IsPhysicalPagePendingForKey(uint32_t page, uint64_t key) const;
    uint32_t GetPendingMeshPageRefCount(uint32_t page, uint64_t key) const;
    void AddPendingMeshPageReference(uint32_t page, uint64_t key);
    void ReleasePendingMeshPageReference(uint32_t page, uint64_t key);
    bool SetGroupResidentBit(uint32_t groupIndex, bool resident);
    void ForceGroupNonResident(uint32_t groupIndex, MeshManager* meshManager, bool clearPageMapEntries);
    void TouchGroupPages(uint32_t groupIndex);
    void PrefetchChildGroupLayouts(uint32_t parentGroupIndex, MeshManager* meshManager);
    void InstallPrefetchedChildGroupLayouts(
        uint32_t parentGroupIndex,
        std::vector<MeshManager::CLodPrefetchedChildLayout>&& prefetchedLayouts);
    void EvictPrefetchedChildLayoutsForOwner(uint32_t ownerGroupIndex);
    void ClearPrefetchedChildLayouts();
    void PollCompletedReadbackSlots();
    void StreamingWorkerMain();
    void ProcessStreamingRequestsBudgeted();
    void RequestStreamingFrameWork();
    void PublishStreamingFrameWorkForFrame();
    void RunStreamingServiceWork();
    bool EnsureParallelSortResources();
    void DestroyParallelSortResources();
    void ClearStreamingUploadFunction(MeshManager* meshManager);
    void InstallStreamingUploadFunction(MeshManager* meshManager);
    bool PublishRetainedUploadBatch();
    void SealStreamingUploadBatch();
    void ObserveUploadBatchTickets();
    void PublishActiveGroupSnapshot();
    void StartStreamingWorker();
    void StopStreamingWorker();

    // Page-level LRU helpers
    void InitializePageLru(MeshManager* meshManager);
    void EnsurePageTrackingCapacity(MeshManager* meshManager);
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
    std::vector<uint32_t> PopFreePages(uint32_t count, MeshManager* meshManager);
    std::vector<uint32_t> PopFreePages(uint32_t count, MeshManager* meshManager, PagePopFailureStats* outStats);
    void ReleaseOwnedPagesForGroup(uint32_t groupIndex, MeshManager* meshManager);
    void ReleaseGroupResidency(uint32_t groupIndex, MeshManager* meshManager, bool clearPageMapEntries);
    void RetirePhysicalPage(uint32_t page, MeshManager* meshManager, bool pinned);
    void DrainRetiredPhysicalPages(MeshManager* meshManager);
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
    bool EvictPhysicalPage(uint32_t page, MeshManager* meshManager);
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

    PreAllocatedPages PreAllocatePagesForGroup(uint32_t groupIndex, const MeshManager::CLodGroupStreamingInfo& info, MeshManager* meshManager);
    bool AssignPagesToGroup(uint32_t groupIndex, const PreAllocatedPages& pages, MeshManager* meshManager);
    void ReleasePreAllocatedPages(const PreAllocatedPages& pages, MeshManager* meshManager);
    bool ValidateRenderableCompletion(
        uint32_t groupIndex,
        const PreAllocatedPages& pages,
        const MeshManager::CLodDiskStreamingCompletion& completion,
        uint32_t expectedPageCount) const;

    std::shared_ptr<Buffer> m_streamingNonResidentBits;
    std::shared_ptr<Buffer> m_streamingActiveGroupsBits;
    std::shared_ptr<Buffer> m_streamingLoadRequestKeys;
    std::shared_ptr<Buffer> m_streamingLoadRequests;
    std::shared_ptr<Buffer> m_streamingLoadCounter;
    std::shared_ptr<Buffer> m_streamingRuntimeState;
    std::shared_ptr<Buffer> m_usedGroupsCounter;
    std::shared_ptr<Buffer> m_usedGroupsBuffer;
    std::shared_ptr<Buffer> m_sourceGroupMismatchCounter;
    std::shared_ptr<Buffer> m_sourceGroupMismatchDetails;

    std::vector<uint32_t> m_streamingNonResidentBitsCpu;
    std::vector<uint32_t> m_streamingActiveGroupsBitsCpu;
    std::vector<uint32_t> m_streamingPinnedGroupsBitsCpu;
    std::vector<uint32_t> m_streamingResidencyInitializedBitsCpu;
    std::vector<uint32_t> m_usedGroupsBitsCpu; // groups reported as visible by the GPU last frame
    std::vector<uint32_t> m_usedGroupsWordsCpu;
    std::vector<uint64_t> m_groupLastUsedTick;
    std::vector<uint32_t> m_parentGroupByGroup;
    std::unordered_map<uint32_t, CachedChildGroupLayout> m_prefetchedChildLayoutsByGroup;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_prefetchedChildLayoutKeysByOwner;
    std::unordered_set<uint32_t> m_errorOverriddenGroups; // groups whose GPU error is currently 0
    CLodPageLRU m_pageLru;
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
    std::unordered_set<uint32_t> m_pendingResidencyCommitGroups;
    std::unordered_map<uint32_t, uint64_t> m_pendingResidencyUploadFenceByGroup;
    std::vector<uint32_t> m_residencyGroupsAwaitingUploadFence;
    std::vector<StreamingRequestState> m_streamingRequestStateByGroup;
    std::vector<uint32_t> m_pendingLoadPriorityByGroup;
    std::vector<PendingStreamingRequest> m_waitingForPagesRequests;
    std::vector<uint32_t> m_waitingForPagesRequestIndexByGroup;
    uint32_t m_waitingForPagesRequestCount = 0u;

    struct StreamingDiagnosticsRecord {
        uint64_t firstRequestTick = 0u;
        uint64_t cpuQueuedTick = 0u;
        uint64_t diskQueuedTick = 0u;
        uint64_t diskCompletedTick = 0u;
        uint64_t uploadQueuedTick = 0u;
        uint64_t commitQueuedTick = 0u;
        uint64_t residentTick = 0u;
        uint64_t uploadedBytes = 0u;
        uint32_t priority = 0u;
        uint32_t duplicateRequests = 0u;
        uint32_t preallocationDeferrals = 0u;
        uint32_t promotionDeferrals = 0u;
        bool active = false;
    };
    std::vector<StreamingDiagnosticsRecord> m_streamingDiagnosticsByGroup;
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
    std::atomic<uint32_t> m_pendingStreamingGpuStorageGroupCapacity{0u};
    std::atomic<uint64_t> m_streamingGpuResizeAckGeneration{0u};
    uint64_t m_observedStreamingGpuResizeAckGeneration = 0u;
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
    std::function<MeshManager*()> m_getMeshManager = []() { return nullptr; };
    std::function<uint32_t()> m_getStreamingCpuUploadBudgetRequests;

    std::vector<PendingStreamingRequest> m_pendingStreamingRequests;
    std::vector<uint32_t> m_pendingStreamingRequestHeapIndexByGroup;
    std::vector<uint32_t> m_pendingStreamingRequestGenerationByGroup;
    CLodPriorityMode m_priorityMode = CLodPriorityMode::Max;
    uint64_t m_streamingDiagnosticTick = 0;

    std::vector<MeshManager::CLodStreamingDomainEvent> m_streamingDomainEventScratch;
    std::vector<uint32_t> m_childGroupsScratch;
    uint64_t m_lastStreamingDomainEventGeneration = 0;
    // A newly-created streaming owner has not consumed the already-live mesh
    // domain. Incremental domain events are not replayable, so its first update
    // must bootstrap residency from MeshManager's authoritative snapshot.
    bool m_streamingDomainFullResetPending = true;

    std::atomic<uint64_t> m_streamingServiceEpoch{1};
    std::atomic<bool> m_streamingServiceRunning{false};
    uint64_t m_streamingServicePublishedGeneration = 0;
    std::vector<uint32_t> m_publishedActiveGroupsBits;
    uint32_t m_publishedActiveGroupScanCount = 0;
    bool m_publishedActiveGroupsBitsUploadPending = true;
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
    rhi::TimelinePtr m_directStorageLaunchFencePtr;
    rhi::Timeline m_directStorageLaunchFenceHandle;
    std::atomic<uint64_t> m_directStorageLaunchFenceCounter{0};
    // Worker publishes launch demand; the graph thread supplies the queue
    // fence, and the worker consumes it after completion.
    std::atomic<bool> m_directStorageLaunchRequested{false};
    std::atomic<uint64_t> m_directStorageArmedLaunchFenceValue{0};

    struct ReadbackStagingSlot {
        enum class State : uint8_t { Free, Recording, Submitted, Decoding };
        std::shared_ptr<Buffer> counterStaging;
        std::shared_ptr<Buffer> requestsStaging;
        std::shared_ptr<Buffer> usedGroupsCounterStaging;
        std::shared_ptr<Buffer> usedGroupsBufferStaging;
        std::shared_ptr<Buffer> sourceGroupMismatchCounterStaging;
        std::shared_ptr<Buffer> sourceGroupMismatchDetailsStaging;
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
            , fenceValue(other.fenceValue)
            , state(other.state.load(std::memory_order_relaxed)) {}
        ReadbackStagingSlot& operator=(ReadbackStagingSlot&& other) noexcept {
            counterStaging = std::move(other.counterStaging);
            requestsStaging = std::move(other.requestsStaging);
            usedGroupsCounterStaging = std::move(other.usedGroupsCounterStaging);
            usedGroupsBufferStaging = std::move(other.usedGroupsBufferStaging);
            sourceGroupMismatchCounterStaging = std::move(other.sourceGroupMismatchCounterStaging);
            sourceGroupMismatchDetailsStaging = std::move(other.sourceGroupMismatchDetailsStaging);
            fenceValue = other.fenceValue;
            state.store(other.state.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };
    std::vector<ReadbackStagingSlot> m_readbackStagingSlots;
    uint32_t m_readbackStagingCursor = 0;
    uint64_t m_readbackSlotFullEvents = 0;

    // Background streaming worker thread
    std::thread m_streamingWorkerThread;
    std::atomic<bool> m_streamingWorkerQuit{false};
    // Decoded (groupIndex, priority) pairs produced by the worker, consumed by the main thread.
    std::vector<std::pair<uint32_t, uint32_t>> m_decodedReadbackBatch;
    // Deduplicated group indices from the GPU used-groups buffer, consumed by the main thread to touch LRU.
    std::vector<uint32_t> m_decodedUsedGroupsBatch;
    uint64_t m_decodedUsedGroupsSampleGeneration = 0;
    uint64_t m_usedGroupsCpuSampleGeneration = 0;
    std::vector<std::pair<uint32_t, uint32_t>> m_readbackBatchScratch;
    std::vector<uint32_t> m_usedGroupsBatchScratch;
    std::vector<uint32_t> m_expiredReadbackGapGroupsScratch;
    std::vector<uint32_t> m_parentChainScratch;
    std::vector<uint32_t> m_lruTouchedGroupsBitsScratch;
    std::vector<uint32_t> m_lruTouchedGroupWordsScratch;
    std::vector<uint32_t> m_protectedGroupsBitsScratch;
    std::vector<uint32_t> m_protectedGroupWordsScratch;
    std::vector<uint32_t> m_decodeSeenGenerationByGroup;
    std::vector<uint32_t> m_decodePriorityAccumByGroup;
    std::vector<uint32_t> m_decodeUsedSeenGenerationByGroup;
    uint32_t m_decodeSeenGeneration = 1u;
    uint32_t m_decodeUsedSeenGeneration = 1u;

    struct ParallelSortState;
    std::unique_ptr<ParallelSortState> m_parallelSortState;
    bool m_parallelSortAvailable = false;
    bool m_parallelSortAttempted = false;

    // Dedicated upload instance + copy queue for async CLod streaming uploads.
    std::unique_ptr<CLodUploadStream> m_uploadStream;
    QueueSlotIndex m_uploadQueueSlot{};
};
