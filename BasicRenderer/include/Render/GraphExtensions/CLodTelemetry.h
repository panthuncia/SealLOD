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

    ClusterCullMeshletIterations,
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
    ObjectCullRejectedPlaneLeft,
    ObjectCullRejectedPlaneRight,
    ObjectCullRejectedPlaneBottom,
    ObjectCullRejectedPlaneTop,
    ObjectCullRejectedPlaneNear,
    ObjectCullRejectedPlaneFar,
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
    TraverseNodesVoxelDescriptorHits,
    TraverseNodesVoxelDescriptorMisses,
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
    VoxelRasterDescriptorMisses,
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

    uint32_t unloadRequested = 0;
    uint32_t unloadUnique = 0;
    uint32_t unloadApplied = 0;
    uint32_t unloadFailed = 0;

    uint32_t residentGroups = 0;
    uint32_t residentAllocations = 0;
    uint32_t queuedRequests = 0;
    uint32_t completedResults = 0;

    uint64_t residentAllocationBytes = 0;
    uint64_t completedResultBytes = 0;
    uint64_t streamedBytesThisFrame = 0;
};

inline std::atomic<uint64_t> g_clodStreamingOperationStatsSequence = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadRequested = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadUnique = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadApplied = 0;
inline std::atomic<uint32_t> g_clodStreamingLoadFailed = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadRequested = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadUnique = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadApplied = 0;
inline std::atomic<uint32_t> g_clodStreamingUnloadFailed = 0;
inline std::atomic<uint32_t> g_clodStreamingResidentGroups = 0;
inline std::atomic<uint32_t> g_clodStreamingResidentAllocations = 0;
inline std::atomic<uint32_t> g_clodStreamingQueuedRequests = 0;
inline std::atomic<uint32_t> g_clodStreamingCompletedResults = 0;
inline std::atomic<uint64_t> g_clodStreamingResidentAllocationBytes = 0;
inline std::atomic<uint64_t> g_clodStreamingCompletedResultBytes = 0;
inline std::atomic<uint64_t> g_clodStreamingStreamedBytesThisFrame = 0;

inline void PublishCLodStreamingOperationStats(const CLodStreamingOperationStats& stats) {
    g_clodStreamingLoadRequested.store(stats.loadRequested, std::memory_order_relaxed);
    g_clodStreamingLoadUnique.store(stats.loadUnique, std::memory_order_relaxed);
    g_clodStreamingLoadApplied.store(stats.loadApplied, std::memory_order_relaxed);
    g_clodStreamingLoadFailed.store(stats.loadFailed, std::memory_order_relaxed);

    g_clodStreamingUnloadRequested.store(stats.unloadRequested, std::memory_order_relaxed);
    g_clodStreamingUnloadUnique.store(stats.unloadUnique, std::memory_order_relaxed);
    g_clodStreamingUnloadApplied.store(stats.unloadApplied, std::memory_order_relaxed);
    g_clodStreamingUnloadFailed.store(stats.unloadFailed, std::memory_order_relaxed);
    g_clodStreamingResidentGroups.store(stats.residentGroups, std::memory_order_relaxed);
    g_clodStreamingResidentAllocations.store(stats.residentAllocations, std::memory_order_relaxed);
    g_clodStreamingQueuedRequests.store(stats.queuedRequests, std::memory_order_relaxed);
    g_clodStreamingCompletedResults.store(stats.completedResults, std::memory_order_relaxed);
    g_clodStreamingResidentAllocationBytes.store(stats.residentAllocationBytes, std::memory_order_relaxed);
    g_clodStreamingCompletedResultBytes.store(stats.completedResultBytes, std::memory_order_relaxed);
    g_clodStreamingStreamedBytesThisFrame.store(stats.streamedBytesThisFrame, std::memory_order_relaxed);

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

    outStats.unloadRequested = g_clodStreamingUnloadRequested.load(std::memory_order_relaxed);
    outStats.unloadUnique = g_clodStreamingUnloadUnique.load(std::memory_order_relaxed);
    outStats.unloadApplied = g_clodStreamingUnloadApplied.load(std::memory_order_relaxed);
    outStats.unloadFailed = g_clodStreamingUnloadFailed.load(std::memory_order_relaxed);

    outStats.residentGroups = g_clodStreamingResidentGroups.load(std::memory_order_relaxed);
    outStats.residentAllocations = g_clodStreamingResidentAllocations.load(std::memory_order_relaxed);
    outStats.queuedRequests = g_clodStreamingQueuedRequests.load(std::memory_order_relaxed);
    outStats.completedResults = g_clodStreamingCompletedResults.load(std::memory_order_relaxed);
    outStats.residentAllocationBytes = g_clodStreamingResidentAllocationBytes.load(std::memory_order_relaxed);
    outStats.completedResultBytes = g_clodStreamingCompletedResultBytes.load(std::memory_order_relaxed);
    outStats.streamedBytesThisFrame = g_clodStreamingStreamedBytesThisFrame.load(std::memory_order_relaxed);

    inOutSequence = sequence;
    return true;
}
