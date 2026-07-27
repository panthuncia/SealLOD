#pragma once

#include <array>
#include <atomic>
#include <cstdint>

enum class CLodWorkGraphCounterIndex : uint32_t {
    ObjectCullThreads = 0,
    ObjectCullInRangeThreads,
    ObjectCullVisibleThreads,
    ObjectCullTraverseRecordsEmitted,

    TraverseNodesThreads,
    TraverseNodesInternalNodeRecords,
    TraverseNodesLeafNodeRecords,
    TraverseNodesCulledNodeRecords,
    TraverseNodesRejectedByErrorRecords,
    TraverseNodesActiveChildThreads,
    TraverseNodesTraverseRecordsEmitted,

    ClusterCullThreads,
    ClusterCullInRangeThreads,
    ClusterCullWaves,
    ClusterCullActiveLanes,
    ClusterCullSurvivingLanes,
    ClusterCullZeroSurvivorWaves,
    ClusterCullVisibleClusterWrites,

    TraverseNodesCoalescedLaunches,
    TraverseNodesCoalescedInputRecords,
    TraverseNodesCoalescedInputCount1,
    TraverseNodesCoalescedInputCount2,
    TraverseNodesCoalescedInputCount3,
    TraverseNodesCoalescedInputCount4,
    TraverseNodesCoalescedInputCount5,
    TraverseNodesCoalescedInputCount6,
    TraverseNodesCoalescedInputCount7,
    TraverseNodesCoalescedInputCount8,

    Phase1OcclusionNodeReplayEnqueueAttempts,
    Phase1OcclusionClusterReplayEnqueueAttempts,

    Phase2ReplayNodeLaunches,
    Phase2ReplayNodeInputRecords,
    Phase2ReplayNodeRecordsEmitted,

    Phase2ReplayMeshletLaunches,
    Phase2ReplayMeshletInputRecords,
    Phase2ReplayMeshletBucketRecordsEmitted,

    Phase2ReplayTraverseRecordsConsumed,
    Phase2ReplayClusterBucketRecordsConsumed,

    SegmentEvaluateThreads,
    SegmentEvaluateSegmentRecords,
    SegmentEvaluateEmitBucketThreads,
    SegmentEvaluateRefinedTraversalThreads,
    SegmentEvaluateNonResidentRefinedChildThreads,
    SegmentEvaluateCoalescedLaunches,
    SegmentEvaluateCoalescedInputRecords,

    ReservedClusterCullMeshletIterations,
    ClusterCullRejectedFrustum,
    ClusterCullRejectedCondition2,
    ClusterCullRejectedOcclusion,
    ClusterCullRejectedOutOfRange,
    ClusterCullRejectedPageBounds,
    ClusterCullRejectedCleanPages,

    ChildPrefilterFrustumCulled,
    ChildPrefilterLodRejected,

    ClusterCullShadowClipmapMisses,
    ClusterCullShadowDirtyRegionHits,

    ObjectCullRejectedFrustum,
    ObjectCullRejectedOcclusion,
    ObjectReplayRejectedOcclusion,
    StreamRequestAttempts,
    StreamRequestRangeRejects,
    StreamResidentHits,
    StreamRequestAppends,
    ObjectCullInvalidBounds,

    ClusterCullShadowDirtyQueries,
    ClusterCullShadowDirtyQueriesClipped,
    ClusterCullShadowDirtyRegionCoarseMipChecks,

    PageJobBuildClustersProcessed,
    PageJobBuildPagesEmitted,
    PageJobBuildFallbackToHW,
    PageJobRasterTrianglesClipped,
    PageJobRasterPixelsWritten,
    PageJobRasterFlagWrites,

    ClassifyContributing,
    ClassifyRoutedHW,
    ClassifyRoutedSW,
    ClassifyRoutedPageJob,
    ClassifyPJRejectReyesDisplacement,
    ClassifyPJRejectAlphaTested,
    ClassifyPJRejectNoClipmapIndex,
    ClassifyPJRejectBelowThreshold,
    ClassifyPJRejectDisabled,
    ClassifyPJRejectAlreadySW,
    ClassifySwDisabled,

    PageJobBuildGroupsLaunched,
    PageJobBuildNoClipmap,
    PageJobBuildPagesScanned,
    PageJobBuildZeroDirtyPages,
    PageJobRasterJobsLaunched,
    PageJobRasterTotalTriangles,
    PageJobRasterTrisDepthReject,
    PageJobRasterTrisBackfaceCull,
    PageJobRasterTrisAABBEmpty,
    PageJobRasterTrisRasterized,
    PageJobRasterPixelsTested,
    PageJobRasterJobsWithPixels,

    PageJobDbgPhysDescriptor,
    PageJobDbgAtlasWidth,
    PageJobDbgAtlasHeight,
    PageJobDbgOobPixels,

    // Keep these appended at the end to match workGraphCulling.hlsl.
    ClusterCullBucketRecordsDispatched,
    ClusterCullDenseExpansionBuckets,
    ClusterCullDenseClustersDispatched,
    TraverseNodesVoxelLeafRecords,
    TraverseNodesVoxelRejectedByErrorRecords,
    TraverseNodesVoxelSegmentPageHits,
    TraverseNodesVoxelSegmentPageMisses,
    TraverseNodesVoxelRasterWorkRecords,
    TraverseNodesVoxelRasterWorkDropped,

    RasterSortHistogramInputs,
    RasterSortHistogramVoxelSkipped,
    RasterSortHistogramReyesSkipped,
    RasterSortHistogramTriangleContributors,
    RasterSortCompactionInputs,
    RasterSortCompactionVoxelSkipped,
    RasterSortCompactionReyesSkipped,
    RasterSortCompactionTriangleEmitted,

    RasterMeshShaderGroups,
    RasterMeshShaderInRange,
    RasterMeshShaderInitFailed,
    RasterMeshShaderOutputTriangles,
    RasterMeshShaderZeroTriangleOutputs,
    RasterMeshShaderInitFailedZeroPageSlab,
    RasterMeshShaderInitFailedMeshletOutOfBounds,
    RasterMeshShaderInitFailedInvalidOutputCounts,
    RasterPixelShaderInvocations,
    RasterPixelScissorRejected,
    RasterPixelTargetBoundsRejected,
    RasterPixelVisibilityWrites,
    RasterPixelVirtualShadowClipmapRejected,
    RasterPixelVirtualShadowPageRejected,
    RasterPixelVirtualShadowWrites,
    RasterMeshShaderSourceGroupMismatch,
    ObjectCullRejectedStaleGeneration,
    VoxelRasterWorkGroups,
    VoxelRasterInvalidPackedCluster,
    VoxelRasterSegmentPageMisses,
    VoxelRasterInvalidCluster,
    VoxelRasterInvalidVoxelWidth,
    VoxelRasterProjectionRejected,
    VoxelRasterScissorRejected,
    VoxelRasterDepthRejected,
    VoxelRasterDdaMisses,
    VoxelRasterVisibilityWrites,
    VoxelRasterProjectedPixels,
    VoxelRasterQueuedPixels,
    VoxelRasterQueueOverflow,
    VoxelRasterNonPositiveDepth,
    VoxelRasterVisibilityWins,
    VoxelRasterVisibilityLosses,
    VoxelObjectCandidates,
    VoxelObjectFrustumRejected,
    VoxelObjectVisible,
    VoxelObjectTraverseRecords,
    VoxelRootInternalRecords,
    VoxelRootLeafRecords,
    AssemblyInstanceRootRecords,
    AssemblyPartInstanceRootRecords,
    AssemblyVoxelLeafRecords,
    ReservedAssemblyVoxelLeafDepth1,
    ReservedAssemblyVoxelLeafDepth2,
    ReservedAssemblyVoxelLeafDepth3,
    ReservedAssemblyVoxelLeafDepth4Plus,
    AssemblyVoxelRejectedByErrorRecords,
    AssemblyVoxelSuppressedByChildRecords,
    AssemblyVoxelNonResidentRecords,
    ReservedAssemblyVoxelNonResidentDepth1,
    ReservedAssemblyVoxelNonResidentDepth2,
    ReservedAssemblyVoxelNonResidentDepth3,
    ReservedAssemblyVoxelNonResidentDepth4Plus,
    AssemblyVoxelRasterWorkRecords,
    ReservedAssemblyVoxelRasterWorkDepth1,
    ReservedAssemblyVoxelRasterWorkDepth2,
    ReservedAssemblyVoxelRasterWorkDepth3,
    ReservedAssemblyVoxelRasterWorkDepth4Plus,
    AssemblyPartTraversalRecords,
    AssemblyPartVoxelLeafRecords,
    AssemblyPartVoxelRasterWorkRecords,
    AssemblyPartTriangleBucketRecords,
    NodeBoundsExplicitEvaluations,
    NodeBoundsExplicitFrustumRejected,
    NodeBoundsOverflowFallbacks,
    NodeBoundsAssemblyFallbacks,
    NodeBoundsInvalidFallbacks,
    MeshletBoundsSkinnedLiveEvaluations,
    MeshletBoundsSkinnedInvalidSlotFallbacks,
    MeshletBoundsSkinnedNoValidBoneFallbacks,
    MeshletBoundsSkinnedFallbackFrustumRejected,
    VoxelRasterRigidWorkGroups,
    VoxelRasterSkinnedWorkGroups,
    VoxelRasterPreparedCubeCandidates,
    VoxelRasterSkinBoneGroups,
    VoxelRasterDistributionBinBase,
    VoxelRasterDistributionBinEnd = VoxelRasterDistributionBinBase + 48,

    TraverseWaves = VoxelRasterDistributionBinEnd,
    TraverseActiveLanes,
    TraverseRigidLanes,
    TraverseSkinnedLanes,
    TraverseInternalOnlyWaves,
    TraverseLeafOnlyWaves,
    TraverseMixedNodeTypeWaves,
    TraverseRigidOnlyWaves,
    TraverseSkinnedOnlyWaves,
    TraverseMixedSkinningWaves,
    TraverseChildLoopNodes,
    TraverseChildLoopSlots,
    TraverseChildRecordsEmitted,
    NodeBoundsExplicitBoneCount,
    NodeBoundsExplicitBoneCount1,
    NodeBoundsExplicitBoneCount2,
    NodeBoundsExplicitBoneCount3To4,
    NodeBoundsExplicitBoneCount5To8,
    NodeBoundsExplicitBoneCount9Plus,
    RasterPixelVirtualShadowDynamicWrites,
    ClusterCullSkinnedContributing,
    ClusterCullSkinnedBlockRecords,
    RasterSortSkinnedHistogramInputs,
    RasterSortSkinnedCompactionEmitted,
    RasterMeshShaderSkinnedGroups,
    RasterPixelVirtualShadowDynamicPageRejected,
    RasterMeshShaderSkinnedOutputTriangles,
    RasterPixelVirtualShadowDynamicInvocations,
    ClusterCullSkinnedBlockCandidates,
    ClusterCullSkinnedBlockMetadataMissing,
    ClusterCullSkinnedBlockRectDisjoint,
    ClusterCullSkinnedDirectActivePageHits,
    ClusterCullSkinnedMetadataFalseNegatives,
    ClusterCullSkinnedRectFalseNegatives,
    ClassifySkinnedRoutedHW,
    ClassifySkinnedRoutedSW,
    ClassifySkinnedRoutedPageJob,
    SoftwareRasterDynamicAttempts,
    SoftwareRasterDynamicPageRejected,
    SoftwareRasterDynamicWrites,
    ObjectCullSkinnedClassified,
    ObjectCullNodeSkinningOnly,
    ObjectCullSkinnedFrustumRejected,
    ObjectCullSkinnedCleanRejected,
    ObjectCullSkinnedEmitted,
    ClassifySkinnedSoftwareRecordsWritten,
    RasterSortSkinnedSoftwareHistogramInputs,
    Count
};

inline constexpr uint32_t CLodWorkGraphCounterCount =
    static_cast<uint32_t>(CLodWorkGraphCounterIndex::Count);

struct CLodWorkGraphTelemetryCounters {
    std::array<uint32_t, CLodWorkGraphCounterCount> counters{};
};

inline constexpr uint32_t CLodSourceGroupMismatchDetailCapacity = 1024u;

struct CLodSourceGroupMismatchDetail {
    uint32_t expectedGroupLocalIndex = 0xFFFFFFFFu;
    uint32_t foundGroupLocalIndex = 0xFFFFFFFFu;
    uint32_t expectedGroupGlobalIndex = 0xFFFFFFFFu;
    uint32_t foundGroupGlobalIndex = 0xFFFFFFFFu;
    uint32_t clodMeshMetadataIndex = 0xFFFFFFFFu;
    uint32_t groupsBase = 0u;
    uint32_t expectedSegmentGlobalIndex = 0xFFFFFFFFu;
    uint32_t expectedSegmentPageIndex = 0xFFFFFFFFu;
    uint32_t expectedSegmentFirstMeshlet = 0u;
    uint32_t expectedSegmentMeshletCount = 0u;
    uint32_t expectedSegmentPageSlabDescriptorIndex = 0u;
    uint32_t expectedSegmentPageSlabByteOffset = 0u;
    uint32_t pageLocalMeshletIndex = 0xFFFFFFFFu;
    uint32_t pageSlabDescriptorIndex = 0u;
    uint32_t pageSlabByteOffset = 0u;
    uint32_t visibleClusterIndex = 0xFFFFFFFFu;
    uint32_t unsortedClusterIndex = 0xFFFFFFFFu;
    uint32_t instanceId = 0xFFFFFFFFu;
    uint32_t viewId = 0xFFFFFFFFu;
    uint32_t bucketMeshletIndex = 0u;
    uint32_t bucketCount = 0u;
    uint32_t pad0 = 0u;
};

static_assert(sizeof(CLodSourceGroupMismatchDetail) == 88u, "CLodSourceGroupMismatchDetail size must match HLSL");

inline constexpr uint32_t CLodDirectionalShadowDebugMaxClipmaps = 16u;

struct CLodDirectionalShadowClipmapDebugEntry {
    uint32_t valid = 0;
    float clipDiameter = 0.0f;
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    int64_t pageOffsetX = 0;
    int64_t pageOffsetY = 0;
    std::array<float, 4> positionWorldSpace{};
};

struct CLodDirectionalShadowDebugSnapshot {
    uint32_t clipmapCount = 0;
    std::array<CLodDirectionalShadowClipmapDebugEntry, CLodDirectionalShadowDebugMaxClipmaps> clipmaps{};
};

inline std::atomic<uint32_t> g_clodWorkGraphTelemetryEnabled = 0u;

inline void SetCLodWorkGraphTelemetryEnabled(bool enabled) {
    g_clodWorkGraphTelemetryEnabled.store(enabled ? 1u : 0u, std::memory_order_relaxed);
}

inline bool IsCLodWorkGraphTelemetryEnabled() {
    return g_clodWorkGraphTelemetryEnabled.load(std::memory_order_relaxed) != 0u;
}

inline std::atomic<uint64_t> g_clodDirectionalShadowDebugSequence = 0;
inline CLodDirectionalShadowDebugSnapshot g_clodDirectionalShadowDebugSnapshot{};

inline void PublishCLodDirectionalShadowDebugSnapshot(const CLodDirectionalShadowDebugSnapshot& snapshot) {
    g_clodDirectionalShadowDebugSnapshot = snapshot;
    g_clodDirectionalShadowDebugSequence.fetch_add(1u, std::memory_order_relaxed);
}

inline bool TryReadCLodDirectionalShadowDebugSnapshot(uint64_t& inOutSequence, CLodDirectionalShadowDebugSnapshot& outSnapshot) {
    const uint64_t sequence = g_clodDirectionalShadowDebugSequence.load(std::memory_order_relaxed);
    if (sequence == inOutSequence) {
        return false;
    }

    outSnapshot = g_clodDirectionalShadowDebugSnapshot;
    inOutSequence = sequence;
    return true;
}

struct CLodStreamingOperationStats {
    uint32_t loadRequested = 0;
    uint32_t loadUnique = 0;
    uint32_t loadApplied = 0;
    uint32_t loadFailed = 0;
    uint32_t decodedRequests = 0;
    uint32_t queuedLoadRequests = 0;
    uint32_t duplicateRequests = 0;

    uint32_t unloadRequested = 0;
    uint32_t unloadUnique = 0;
    uint32_t unloadApplied = 0;
    uint32_t unloadFailed = 0;

    uint32_t pendingCpuRequests = 0;
    uint32_t pendingCpuHeapRequests = 0;
    uint32_t waitingForPagesRequests = 0;
    uint32_t inProgressRequests = 0;
    uint32_t diskIoRequests = 0;
    uint32_t pendingCommitGroups = 0;
    uint32_t readyCompletions = 0;
    uint32_t preallocationDeferrals = 0;
    uint32_t promotionDeferrals = 0;
    uint32_t completionSuccess = 0;
    uint32_t completionFailed = 0;
    uint32_t uploadQueuedGroups = 0;

    uint32_t residentGroups = 0;
    uint32_t residentAllocations = 0;
    uint32_t queuedRequests = 0;
    uint32_t queuedOrInFlightGroups = 0;
    uint32_t dispatchedOrInFlightGroups = 0;
    uint32_t completedResults = 0;
    uint32_t pendingDirectStorageLaunches = 0;
    uint32_t pendingDirectStorageUploads = 0;

    uint64_t residentAllocationBytes = 0;
    uint64_t completedResultBytes = 0;
    uint64_t streamedBytesThisFrame = 0;
    uint64_t uploadQueuedBytes = 0;

    uint32_t requestToUploadSamples = 0;
    uint32_t requestToUploadAvgTicks = 0;
    uint32_t requestToUploadWorstTicks = 0;
    uint32_t requestToUploadWorstGroup = 0;
    uint32_t requestToResidentSamples = 0;
    uint32_t requestToResidentAvgTicks = 0;
    uint32_t requestToResidentWorstTicks = 0;
    uint32_t requestToResidentWorstGroup = 0;
    uint32_t diskQueueToCompleteAvgTicks = 0;
    uint32_t diskQueueToCompleteWorstTicks = 0;
    uint32_t uploadToResidentAvgTicks = 0;
    uint32_t uploadToResidentWorstTicks = 0;
    uint32_t commitToResidentAvgTicks = 0;
    uint32_t commitToResidentWorstTicks = 0;
    uint32_t pendingCpuMaxAgeTicks = 0;
    uint32_t pendingCpuMaxAgeGroup = 0;
    uint32_t diskIoMaxAgeTicks = 0;
    uint32_t diskIoMaxAgeGroup = 0;
    uint32_t pendingCommitMaxAgeTicks = 0;
    uint32_t pendingCommitMaxAgeGroup = 0;
};

inline std::atomic<uint64_t> g_clodStreamingOperationStatsSequence = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadRequested = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadUnique = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadApplied = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadFailed = 0;
inline std::atomic<uint32_t> g_clodStreamingDecodedRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingQueuedLoadRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingDuplicateRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadRequested = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadUnique = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadApplied = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadFailed = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCpuRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCpuHeapRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingWaitingForPagesRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingInProgressRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingDiskIoRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCommitGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingReadyCompletions = 0;
inline std::atomic<uint32_t> g_clodStreamingPreallocationDeferrals = 0;
inline std::atomic<uint32_t> g_clodStreamingPromotionDeferrals = 0;
inline std::atomic<uint32_t> g_clodStreamingCompletionSuccess = 0;
inline std::atomic<uint32_t> g_clodStreamingCompletionFailed = 0;
inline std::atomic<uint32_t> g_clodStreamingUploadQueuedGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingResidentGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingResidentAllocations = 0;
inline std::atomic<uint32_t> g_clodStreamingQueuedRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingQueuedOrInFlightGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingDispatchedOrInFlightGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingCompletedResults = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingDirectStorageLaunches = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingDirectStorageUploads = 0;
inline std::atomic<uint64_t> g_clodStreamingResidentAllocationBytes = 0;
inline std::atomic<uint64_t> g_clodStreamingCompletedResultBytes = 0;
inline std::atomic<uint64_t> g_clodStreamingStreamedBytesThisFrame = 0;
inline std::atomic<uint64_t> g_clodStreamingUploadQueuedBytes = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToUploadSamples = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToUploadAvgTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToUploadWorstTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToUploadWorstGroup = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToResidentSamples = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToResidentAvgTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToResidentWorstTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingRequestToResidentWorstGroup = 0;
inline std::atomic<uint32_t> g_clodStreamingDiskQueueToCompleteAvgTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingDiskQueueToCompleteWorstTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingUploadToResidentAvgTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingUploadToResidentWorstTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingCommitToResidentAvgTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingCommitToResidentWorstTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCpuMaxAgeTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCpuMaxAgeGroup = 0;
inline std::atomic<uint32_t> g_clodStreamingDiskIoMaxAgeTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingDiskIoMaxAgeGroup = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCommitMaxAgeTicks = 0;
inline std::atomic<uint32_t> g_clodStreamingPendingCommitMaxAgeGroup = 0;

inline void PublishCLodStreamingOperationStats(const CLodStreamingOperationStats& stats) {
    g_clodStreamingLoadRequested.store(stats.loadRequested, std::memory_order_relaxed);
    g_clodStreamingLoadUnique.store(stats.loadUnique, std::memory_order_relaxed);
    g_clodStreamingLoadApplied.store(stats.loadApplied, std::memory_order_relaxed);
    g_clodStreamingLoadFailed.store(stats.loadFailed, std::memory_order_relaxed);
    g_clodStreamingDecodedRequests.store(stats.decodedRequests, std::memory_order_relaxed);
    g_clodStreamingQueuedLoadRequests.store(stats.queuedLoadRequests, std::memory_order_relaxed);
    g_clodStreamingDuplicateRequests.store(stats.duplicateRequests, std::memory_order_relaxed);

    g_clodStreamingUnloadRequested.store(stats.unloadRequested, std::memory_order_relaxed);
    g_clodStreamingUnloadUnique.store(stats.unloadUnique, std::memory_order_relaxed);
    g_clodStreamingUnloadApplied.store(stats.unloadApplied, std::memory_order_relaxed);
    g_clodStreamingUnloadFailed.store(stats.unloadFailed, std::memory_order_relaxed);
    g_clodStreamingPendingCpuRequests.store(stats.pendingCpuRequests, std::memory_order_relaxed);
    g_clodStreamingPendingCpuHeapRequests.store(stats.pendingCpuHeapRequests, std::memory_order_relaxed);
    g_clodStreamingWaitingForPagesRequests.store(stats.waitingForPagesRequests, std::memory_order_relaxed);
    g_clodStreamingInProgressRequests.store(stats.inProgressRequests, std::memory_order_relaxed);
    g_clodStreamingDiskIoRequests.store(stats.diskIoRequests, std::memory_order_relaxed);
    g_clodStreamingPendingCommitGroups.store(stats.pendingCommitGroups, std::memory_order_relaxed);
    g_clodStreamingReadyCompletions.store(stats.readyCompletions, std::memory_order_relaxed);
    g_clodStreamingPreallocationDeferrals.store(stats.preallocationDeferrals, std::memory_order_relaxed);
    g_clodStreamingPromotionDeferrals.store(stats.promotionDeferrals, std::memory_order_relaxed);
    g_clodStreamingCompletionSuccess.store(stats.completionSuccess, std::memory_order_relaxed);
    g_clodStreamingCompletionFailed.store(stats.completionFailed, std::memory_order_relaxed);
    g_clodStreamingUploadQueuedGroups.store(stats.uploadQueuedGroups, std::memory_order_relaxed);
    g_clodStreamingResidentGroups.store(stats.residentGroups, std::memory_order_relaxed);
    g_clodStreamingResidentAllocations.store(stats.residentAllocations, std::memory_order_relaxed);
    g_clodStreamingQueuedRequests.store(stats.queuedRequests, std::memory_order_relaxed);
    g_clodStreamingQueuedOrInFlightGroups.store(stats.queuedOrInFlightGroups, std::memory_order_relaxed);
    g_clodStreamingDispatchedOrInFlightGroups.store(stats.dispatchedOrInFlightGroups, std::memory_order_relaxed);
    g_clodStreamingCompletedResults.store(stats.completedResults, std::memory_order_relaxed);
    g_clodStreamingPendingDirectStorageLaunches.store(stats.pendingDirectStorageLaunches, std::memory_order_relaxed);
    g_clodStreamingPendingDirectStorageUploads.store(stats.pendingDirectStorageUploads, std::memory_order_relaxed);
    g_clodStreamingResidentAllocationBytes.store(stats.residentAllocationBytes, std::memory_order_relaxed);
    g_clodStreamingCompletedResultBytes.store(stats.completedResultBytes, std::memory_order_relaxed);
    g_clodStreamingStreamedBytesThisFrame.store(stats.streamedBytesThisFrame, std::memory_order_relaxed);
    g_clodStreamingUploadQueuedBytes.store(stats.uploadQueuedBytes, std::memory_order_relaxed);
    g_clodStreamingRequestToUploadSamples.store(stats.requestToUploadSamples, std::memory_order_relaxed);
    g_clodStreamingRequestToUploadAvgTicks.store(stats.requestToUploadAvgTicks, std::memory_order_relaxed);
    g_clodStreamingRequestToUploadWorstTicks.store(stats.requestToUploadWorstTicks, std::memory_order_relaxed);
    g_clodStreamingRequestToUploadWorstGroup.store(stats.requestToUploadWorstGroup, std::memory_order_relaxed);
    g_clodStreamingRequestToResidentSamples.store(stats.requestToResidentSamples, std::memory_order_relaxed);
    g_clodStreamingRequestToResidentAvgTicks.store(stats.requestToResidentAvgTicks, std::memory_order_relaxed);
    g_clodStreamingRequestToResidentWorstTicks.store(stats.requestToResidentWorstTicks, std::memory_order_relaxed);
    g_clodStreamingRequestToResidentWorstGroup.store(stats.requestToResidentWorstGroup, std::memory_order_relaxed);
    g_clodStreamingDiskQueueToCompleteAvgTicks.store(stats.diskQueueToCompleteAvgTicks, std::memory_order_relaxed);
    g_clodStreamingDiskQueueToCompleteWorstTicks.store(stats.diskQueueToCompleteWorstTicks, std::memory_order_relaxed);
    g_clodStreamingUploadToResidentAvgTicks.store(stats.uploadToResidentAvgTicks, std::memory_order_relaxed);
    g_clodStreamingUploadToResidentWorstTicks.store(stats.uploadToResidentWorstTicks, std::memory_order_relaxed);
    g_clodStreamingCommitToResidentAvgTicks.store(stats.commitToResidentAvgTicks, std::memory_order_relaxed);
    g_clodStreamingCommitToResidentWorstTicks.store(stats.commitToResidentWorstTicks, std::memory_order_relaxed);
    g_clodStreamingPendingCpuMaxAgeTicks.store(stats.pendingCpuMaxAgeTicks, std::memory_order_relaxed);
    g_clodStreamingPendingCpuMaxAgeGroup.store(stats.pendingCpuMaxAgeGroup, std::memory_order_relaxed);
    g_clodStreamingDiskIoMaxAgeTicks.store(stats.diskIoMaxAgeTicks, std::memory_order_relaxed);
    g_clodStreamingDiskIoMaxAgeGroup.store(stats.diskIoMaxAgeGroup, std::memory_order_relaxed);
    g_clodStreamingPendingCommitMaxAgeTicks.store(stats.pendingCommitMaxAgeTicks, std::memory_order_relaxed);
    g_clodStreamingPendingCommitMaxAgeGroup.store(stats.pendingCommitMaxAgeGroup, std::memory_order_relaxed);

    g_clodStreamingOperationStatsSequence.fetch_add(1u, std::memory_order_relaxed);
}

inline bool TryReadCLodStreamingOperationStats(uint64_t& inOutSequence, CLodStreamingOperationStats& outStats) {
    const uint64_t sequence = g_clodStreamingOperationStatsSequence.load(std::memory_order_relaxed);
    if (sequence == inOutSequence) {
        return false;
    }

    outStats.loadRequested = g_clodStreamingLoadRequested.load(std::memory_order_relaxed);
    outStats.loadUnique = g_clodStreamingLoadUnique.load(std::memory_order_relaxed);
    outStats.loadApplied = g_clodStreamingLoadApplied.load(std::memory_order_relaxed);
    outStats.loadFailed = g_clodStreamingLoadFailed.load(std::memory_order_relaxed);
    outStats.decodedRequests = g_clodStreamingDecodedRequests.load(std::memory_order_relaxed);
    outStats.queuedLoadRequests = g_clodStreamingQueuedLoadRequests.load(std::memory_order_relaxed);
    outStats.duplicateRequests = g_clodStreamingDuplicateRequests.load(std::memory_order_relaxed);

    outStats.unloadRequested = g_clodStreamingUnloadRequested.load(std::memory_order_relaxed);
    outStats.unloadUnique = g_clodStreamingUnloadUnique.load(std::memory_order_relaxed);
    outStats.unloadApplied = g_clodStreamingUnloadApplied.load(std::memory_order_relaxed);
    outStats.unloadFailed = g_clodStreamingUnloadFailed.load(std::memory_order_relaxed);

    outStats.pendingCpuRequests = g_clodStreamingPendingCpuRequests.load(std::memory_order_relaxed);
    outStats.pendingCpuHeapRequests = g_clodStreamingPendingCpuHeapRequests.load(std::memory_order_relaxed);
    outStats.waitingForPagesRequests = g_clodStreamingWaitingForPagesRequests.load(std::memory_order_relaxed);
    outStats.inProgressRequests = g_clodStreamingInProgressRequests.load(std::memory_order_relaxed);
    outStats.diskIoRequests = g_clodStreamingDiskIoRequests.load(std::memory_order_relaxed);
    outStats.pendingCommitGroups = g_clodStreamingPendingCommitGroups.load(std::memory_order_relaxed);
    outStats.readyCompletions = g_clodStreamingReadyCompletions.load(std::memory_order_relaxed);
    outStats.preallocationDeferrals = g_clodStreamingPreallocationDeferrals.load(std::memory_order_relaxed);
    outStats.promotionDeferrals = g_clodStreamingPromotionDeferrals.load(std::memory_order_relaxed);
    outStats.completionSuccess = g_clodStreamingCompletionSuccess.load(std::memory_order_relaxed);
    outStats.completionFailed = g_clodStreamingCompletionFailed.load(std::memory_order_relaxed);
    outStats.uploadQueuedGroups = g_clodStreamingUploadQueuedGroups.load(std::memory_order_relaxed);
    outStats.residentGroups = g_clodStreamingResidentGroups.load(std::memory_order_relaxed);
    outStats.residentAllocations = g_clodStreamingResidentAllocations.load(std::memory_order_relaxed);
    outStats.queuedRequests = g_clodStreamingQueuedRequests.load(std::memory_order_relaxed);
    outStats.queuedOrInFlightGroups = g_clodStreamingQueuedOrInFlightGroups.load(std::memory_order_relaxed);
    outStats.dispatchedOrInFlightGroups = g_clodStreamingDispatchedOrInFlightGroups.load(std::memory_order_relaxed);
    outStats.completedResults = g_clodStreamingCompletedResults.load(std::memory_order_relaxed);
    outStats.pendingDirectStorageLaunches = g_clodStreamingPendingDirectStorageLaunches.load(std::memory_order_relaxed);
    outStats.pendingDirectStorageUploads = g_clodStreamingPendingDirectStorageUploads.load(std::memory_order_relaxed);
    outStats.residentAllocationBytes = g_clodStreamingResidentAllocationBytes.load(std::memory_order_relaxed);
    outStats.completedResultBytes = g_clodStreamingCompletedResultBytes.load(std::memory_order_relaxed);
    outStats.streamedBytesThisFrame = g_clodStreamingStreamedBytesThisFrame.load(std::memory_order_relaxed);
    outStats.uploadQueuedBytes = g_clodStreamingUploadQueuedBytes.load(std::memory_order_relaxed);
    outStats.requestToUploadSamples = g_clodStreamingRequestToUploadSamples.load(std::memory_order_relaxed);
    outStats.requestToUploadAvgTicks = g_clodStreamingRequestToUploadAvgTicks.load(std::memory_order_relaxed);
    outStats.requestToUploadWorstTicks = g_clodStreamingRequestToUploadWorstTicks.load(std::memory_order_relaxed);
    outStats.requestToUploadWorstGroup = g_clodStreamingRequestToUploadWorstGroup.load(std::memory_order_relaxed);
    outStats.requestToResidentSamples = g_clodStreamingRequestToResidentSamples.load(std::memory_order_relaxed);
    outStats.requestToResidentAvgTicks = g_clodStreamingRequestToResidentAvgTicks.load(std::memory_order_relaxed);
    outStats.requestToResidentWorstTicks = g_clodStreamingRequestToResidentWorstTicks.load(std::memory_order_relaxed);
    outStats.requestToResidentWorstGroup = g_clodStreamingRequestToResidentWorstGroup.load(std::memory_order_relaxed);
    outStats.diskQueueToCompleteAvgTicks = g_clodStreamingDiskQueueToCompleteAvgTicks.load(std::memory_order_relaxed);
    outStats.diskQueueToCompleteWorstTicks = g_clodStreamingDiskQueueToCompleteWorstTicks.load(std::memory_order_relaxed);
    outStats.uploadToResidentAvgTicks = g_clodStreamingUploadToResidentAvgTicks.load(std::memory_order_relaxed);
    outStats.uploadToResidentWorstTicks = g_clodStreamingUploadToResidentWorstTicks.load(std::memory_order_relaxed);
    outStats.commitToResidentAvgTicks = g_clodStreamingCommitToResidentAvgTicks.load(std::memory_order_relaxed);
    outStats.commitToResidentWorstTicks = g_clodStreamingCommitToResidentWorstTicks.load(std::memory_order_relaxed);
    outStats.pendingCpuMaxAgeTicks = g_clodStreamingPendingCpuMaxAgeTicks.load(std::memory_order_relaxed);
    outStats.pendingCpuMaxAgeGroup = g_clodStreamingPendingCpuMaxAgeGroup.load(std::memory_order_relaxed);
    outStats.diskIoMaxAgeTicks = g_clodStreamingDiskIoMaxAgeTicks.load(std::memory_order_relaxed);
    outStats.diskIoMaxAgeGroup = g_clodStreamingDiskIoMaxAgeGroup.load(std::memory_order_relaxed);
    outStats.pendingCommitMaxAgeTicks = g_clodStreamingPendingCommitMaxAgeTicks.load(std::memory_order_relaxed);
    outStats.pendingCommitMaxAgeGroup = g_clodStreamingPendingCommitMaxAgeGroup.load(std::memory_order_relaxed);

    inOutSequence = sequence;
    return true;
}
