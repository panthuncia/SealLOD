#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/PublishedRendererState.h>

namespace br::render {
struct SamplingReadinessSnapshot {
    struct SchedulerDomainSnapshot {
        uint64_t queued = 0, active = 0, completed = 0;
        uint64_t queueWaitMicros = 0, maxQueueWaitMicros = 0;
        uint64_t executionMicros = 0, maxExecutionMicros = 0;
        uint64_t highWatermark = 0;
        uint32_t concurrency = 0;
        std::string maxQueueWaitTask;
        std::string maxExecutionTask;
    };
    bool sceneTaskInFlight = false;
    bool hasCommittedSceneSnapshot = false;
    uint64_t committedSceneSnapshotSequence = 0;
    uint64_t pendingSceneSnapshotSequence = 0;
    uint32_t pendingTextureReloads = 0;
    uint32_t fullResolutionTextures = 0;
    uint32_t materialTextures = 0;
    uint32_t streamableMaterialTextures = 0;
    uint32_t streamingEnabledMaterialTextures = 0;
    uint32_t streamableFullResolutionTextures = 0;
    uint64_t materialTextureResidentBytes = 0;
    uint64_t streamableMaterialTextureResidentBytes = 0;
    std::vector<uint32_t> materialTextureResidentTopMipHistogram;
    std::vector<uint32_t> materialTextureRequestedTopMipHistogram;
    std::vector<uint32_t> materialTextureFeedbackTopMipHistogram;
    uint32_t materialTexturesWithoutFeedback = 0;
    std::vector<uint64_t> materialTextureResidentBytesByTopMip;
    uint32_t materialTextureResidentShapeMismatchCount = 0;
    uint64_t materialTextureResidentShapeMismatchBytes = 0;
    uint32_t materialTextureDistinctPreparedCount = 0;
    uint64_t materialTextureDistinctPreparedBytes = 0;
    uint32_t activeMaterialTextureResourceCount = 0;
    uint64_t activeMaterialTextureResourceBytes = 0;
    uint32_t externallyManagedActiveTextureResourceCount = 0;
    uint64_t externallyManagedActiveTextureResourceBytes = 0;
		uint32_t graphManagedParticipatingActiveTextureResourceCount = 0;
		uint64_t graphManagedParticipatingActiveTextureResourceBytes = 0;
    uint32_t alphaTestedMaterialTextureCount = 0;
    uint32_t alphaTestedMaterialTextureMipCapViolationCount = 0;
    std::vector<uint64_t> materialTexturePublishedResourceIDs;
    std::vector<std::string> largestMaterialTextureRecords;
    uint64_t deferredGpuReleaseCount = 0;
    uint64_t deferredGpuReleaseResourceCount = 0;
    uint64_t blockedGpuReleaseCount = 0;
    uint64_t invalidGpuReleaseTimelineCount = 0;
    uint64_t deviceErrorGpuReleaseTimelineCount = 0;
    uint64_t incompleteGpuReleaseTimelineCount = 0;
    std::vector<uint64_t> deferredGpuReleaseResourceIDs;
    uint64_t deletionQueueObjectCount = 0;
    uint64_t deletionQueueAllocationCount = 0;
    uint64_t deletionQueueTrackedAllocationCount = 0;
    uint32_t residentClodGroups = 0;
    uint32_t residentClodAllocations = 0;
    uint64_t residentClodAllocationBytes = 0;
    uint64_t totalClodStreamedBytes = 0;
    uint32_t queuedClodRequests = 0;
    uint32_t inFlightClodGroups = 0;
    uint32_t completedClodResults = 0;
    uint32_t pendingDirectStorageLaunches = 0;
    uint32_t pendingDirectStorageUploads = 0;
    uint32_t ioTasks = 0;
    uint32_t backgroundTasks = 0;
    uint32_t shaderCompileTasks = 0;
    uint32_t schedulerWorkerCount = 0;
    std::array<SchedulerDomainSnapshot, static_cast<std::size_t>(TaskDomain::Count)> schedulerDomains{};
    br::render::RendererStatePublisherStats rendererStatePublisher;
    uint64_t deferredRetireQueueDepth = 0;
    uint64_t drawRecordsAllocated = 0;
};
} // namespace br::render
