#include <BasicRenderer/Renderer.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"
#include "Runtime/Publication/PersistentRendererPublication.h"
#include "Materials/MaterialManager.h"
#include "VirtualGeometry/GeometryStorage/MeshManager.h"
#include "Scene/Objects/ObjectManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Runtime/Device/DeletionManager.h"
#include "Runtime/Device/DescriptorHeapManager.h"

#include <BasicTelemetry/Tracy.h>
#include <d3d12.h>
#include <spdlog/spdlog.h>
#include "Diagnostics/Telemetry/FrameTaskGraphTelemetry.h"
#include <sstream>
#include <utility>

br::render::ObjectStorageStats Renderer::GetObjectManagerStats() const {
    return m_pObjectManager ? m_pObjectManager->GetStats() : br::render::ObjectStorageStats{};
}

Renderer::SamplingReadinessSnapshot Renderer::GetSamplingReadinessSnapshot(bool includeExpensiveDiagnostics) const {
    SamplingReadinessSnapshot snapshot;
    const auto sceneStatus = GetSceneOverlapStatus();
    snapshot.sceneTaskInFlight = sceneStatus.taskInFlight;
    snapshot.hasCommittedSceneSnapshot = sceneStatus.hasCommittedSnapshot;
    snapshot.committedSceneSnapshotSequence = sceneStatus.committedSnapshotSequence;
    snapshot.pendingSceneSnapshotSequence = sceneStatus.pendingSnapshotSequence;

    if (m_pMaterialManager && !includeExpensiveDiagnostics) {
        const auto textureStats = m_pMaterialManager->GetMaterialTextureStreamingReadinessStats();
        snapshot.pendingTextureReloads = textureStats.pendingReloadTextureCount;
        snapshot.fullResolutionTextures = textureStats.fullResolutionResidentTextureCount;
    }
    else if (m_pMaterialManager) {
        const auto textureStats = m_pMaterialManager->GetMaterialTextureStreamingStats();
        snapshot.pendingTextureReloads = textureStats.pendingReloadTextureCount;
        snapshot.fullResolutionTextures = textureStats.fullResolutionResidentTextureCount;
        snapshot.materialTextures = textureStats.uniqueMaterialTextureCount;
        snapshot.streamableMaterialTextures = textureStats.uniqueStreamableTextureCount;
        snapshot.streamingEnabledMaterialTextures = textureStats.uniqueStreamingEnabledTextureCount;
        snapshot.streamableFullResolutionTextures = textureStats.streamableFullResolutionResidentTextureCount;
        snapshot.materialTextureResidentBytes = textureStats.totalResidentBytes;
        snapshot.streamableMaterialTextureResidentBytes = textureStats.streamableResidentBytes;
        snapshot.materialTextureResidentTopMipHistogram = textureStats.residentTopMipHistogram;
        snapshot.materialTextureRequestedTopMipHistogram = textureStats.requestedTopMipHistogram;
        snapshot.materialTextureFeedbackTopMipHistogram = textureStats.feedbackTopMipHistogram;
        snapshot.materialTexturesWithoutFeedback = textureStats.texturesWithoutFeedback;
        snapshot.materialTextureResidentBytesByTopMip = textureStats.residentBytesByTopMip;
        snapshot.materialTextureResidentShapeMismatchCount = textureStats.residentShapeMismatchTextureCount;
        snapshot.materialTextureResidentShapeMismatchBytes = textureStats.residentShapeMismatchBytes;
        snapshot.materialTextureDistinctPreparedCount = textureStats.distinctPreparedTextureCount;
        snapshot.materialTextureDistinctPreparedBytes = textureStats.distinctPreparedTextureBytes;
        snapshot.activeMaterialTextureResourceCount = textureStats.activeMaterialResourceCount;
        snapshot.activeMaterialTextureResourceBytes = textureStats.activeMaterialResourceBytes;
        snapshot.externallyManagedActiveTextureResourceCount = textureStats.externallyManagedActiveResourceCount;
        snapshot.externallyManagedActiveTextureResourceBytes = textureStats.externallyManagedActiveResourceBytes;
		snapshot.graphManagedParticipatingActiveTextureResourceCount = textureStats.graphManagedParticipatingActiveResourceCount;
		snapshot.graphManagedParticipatingActiveTextureResourceBytes = textureStats.graphManagedParticipatingActiveResourceBytes;
        snapshot.alphaTestedMaterialTextureCount = textureStats.alphaTestedTextureCount;
        snapshot.alphaTestedMaterialTextureMipCapViolationCount = textureStats.alphaTestedMipCapViolationCount;
        snapshot.materialTexturePublishedResourceIDs = textureStats.publishedResourceIDs;
        snapshot.largestMaterialTextureRecords.reserve(textureStats.largestResidentTextures.size());
        for (const auto& record : textureStats.largestResidentTextures) {
            std::ostringstream stream;
            stream
                << "streaming_id=" << record.streamingTextureID
                << " descriptor=" << record.imageDescriptorIndex
                << " resource=" << record.imageResourceID
                << " bytes=" << record.residentBytes
                << " resident_dimensions=" << record.residentWidth << "x" << record.residentHeight
                << " expected_resident_dimensions=" << record.expectedResidentWidth << "x" << record.expectedResidentHeight
                << " total_mips=" << record.totalMipCount
                << " resident_top_mip=" << record.residentTopMip
                << " resident_mip_count=" << record.residentMipCount
                << " requested_top_mip=" << record.requestedTopMip
                << " feedback_top_mip=";
            if (record.feedbackTopMip == UINT32_MAX) {
                stream << "none";
            }
            else {
                stream << record.feedbackTopMip;
            }
            stream
                << " eligible=" << (record.eligible ? 1 : 0)
                << " enabled=" << (record.enabled ? 1 : 0)
                << " alpha_tested=" << (record.alphaTested ? 1 : 0)
                << " identifier=\"" << record.identifier << "\"";
            snapshot.largestMaterialTextureRecords.push_back(stream.str());
        }
    }
    if (m_pMeshManager) {
        const auto clodStats = m_pMeshManager->GetCLodStreamingDebugStats();
        snapshot.residentClodGroups = clodStats.residentGroups;
        snapshot.residentClodAllocations = clodStats.residentAllocations;
        snapshot.residentClodAllocationBytes = clodStats.residentAllocationBytes;
        snapshot.totalClodStreamedBytes = clodStats.totalStreamedBytes;
        snapshot.queuedClodRequests = clodStats.queuedRequests;
        snapshot.inFlightClodGroups = clodStats.queuedOrInFlightGroups + clodStats.dispatchedOrInFlightGroups;
        snapshot.completedClodResults = clodStats.completedResults;
        snapshot.pendingDirectStorageLaunches = clodStats.pendingDirectStorageLaunches;
        snapshot.pendingDirectStorageUploads = clodStats.pendingDirectStorageUploads;
    }
    const auto taskStats = TaskSchedulerManager::GetInstance().GetQueueStats();
    snapshot.schedulerWorkerCount = TaskSchedulerManager::GetInstance().WorkerCount();
    for (std::size_t index = 0; index < snapshot.schedulerDomains.size(); ++index) {
        const auto& source = taskStats.domains[index];
        auto& target = snapshot.schedulerDomains[index];
        target.queued = source.queued;
        target.active = source.active;
        target.completed = source.completed;
        target.queueWaitMicros = source.queueWaitMicros;
        target.maxQueueWaitMicros = source.maxQueueWaitMicros;
        target.executionMicros = source.executionMicros;
        target.maxExecutionMicros = source.maxExecutionMicros;
        target.highWatermark = source.highWatermark;
        target.concurrency = TaskSchedulerManager::GetInstance().DomainConcurrency(
            static_cast<TaskDomain>(index));
        target.maxQueueWaitTask = source.maxQueueWaitTask;
        target.maxExecutionTask = source.maxExecutionTask;
    }
    if (m_rendererStatePublisher) {
        snapshot.rendererStatePublisher = m_rendererStatePublisher->Stats();
    }
    snapshot.ioTasks = taskStats.ioQueued + taskStats.ioActive;
    snapshot.backgroundTasks = taskStats.backgroundQueued + taskStats.backgroundActive;
    snapshot.shaderCompileTasks = taskStats.shaderCompileQueued + taskStats.shaderCompileActive;
    if (includeExpensiveDiagnostics) {
        const auto deferredReleaseStats = org::DescriptorHeapManager::GetInstance().GetDeferredReleaseStats();
        snapshot.deferredGpuReleaseCount = deferredReleaseStats.releaseCount;
        snapshot.deferredGpuReleaseResourceCount = deferredReleaseStats.resourceCount;
        snapshot.blockedGpuReleaseCount = deferredReleaseStats.blockedReleaseCount;
        snapshot.invalidGpuReleaseTimelineCount = deferredReleaseStats.invalidTimelineCount;
        snapshot.deviceErrorGpuReleaseTimelineCount = deferredReleaseStats.deviceErrorTimelineCount;
        snapshot.incompleteGpuReleaseTimelineCount = deferredReleaseStats.incompleteTimelineCount;
        snapshot.deferredGpuReleaseResourceIDs = std::move(deferredReleaseStats.resourceIDs);
        const auto deletionStats = org::DeletionManager::GetInstance().GetStats();
        snapshot.deletionQueueObjectCount = deletionStats.objectCount;
        snapshot.deletionQueueAllocationCount = deletionStats.allocationCount;
        snapshot.deletionQueueTrackedAllocationCount = deletionStats.trackedAllocationCount;
    }
    if (m_pObjectManager) {
        const auto objectStats = m_pObjectManager->GetStats();
        snapshot.deferredRetireQueueDepth = objectStats.deferredRetireQueueDepth;
        snapshot.drawRecordsAllocated = objectStats.instanceDrawRecordsAllocated;
    }
    return snapshot;
}

void Renderer::BeginFrameTaskGraphCapture() {
    br::telemetry::BeginFrameTaskGraphCapture(m_totalFramesRendered, m_frameIndex);
    m_lastFrameTaskNodeIndex = -1;
}

void Renderer::RecordFrameTaskStage(
    const char* stageName,
    br::telemetry::CpuTaskDomain domain,
    const std::chrono::steady_clock::time_point& stageStart,
    const std::chrono::steady_clock::time_point& stageEnd) {
    m_lastFrameTaskNodeIndex = br::telemetry::RecordFrameTaskNode(stageName, domain, m_lastFrameTaskNodeIndex, stageStart, stageEnd);
}

void Renderer::PublishFrameTaskGraphCapture() {
    br::telemetry::PublishFrameTaskGraphSnapshot();
}


void Renderer::StartAsyncStateGraphTrace(br::render::AsyncStateGraphTraceConfig config) {
    m_pendingAsyncStateGraphTrace = config;
    if (m_asyncStateGraph) m_asyncStateGraph->StartTrace(config);
}

bool Renderer::AsyncStateGraphTraceActive() const {
    return m_asyncStateGraph && m_asyncStateGraph->TraceActive();
}

br::render::AsyncStateGraphTraceReport Renderer::StopAsyncStateGraphTraceAndWriteReport(
    const std::filesystem::path& outputDirectory) {
    m_pendingAsyncStateGraphTrace.reset();
    return m_asyncStateGraph
        ? m_asyncStateGraph->StopTraceAndWriteReport(outputDirectory)
        : br::render::AsyncStateGraphTraceReport{};
}


void D3D12DebugCallback(
    D3D12_MESSAGE_CATEGORY Category,
    D3D12_MESSAGE_SEVERITY Severity,
    D3D12_MESSAGE_ID ID,
    LPCSTR pDescription,
    void* pContext) {
    std::string message(pDescription);

    // Redirect messages to spdlog based on severity
    switch (Severity) {
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        spdlog::critical("D3D12 CORRUPTION: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_ERROR:
        spdlog::error("D3D12 ERROR: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_WARNING:
        spdlog::warn("D3D12 WARNING: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_INFO:
        spdlog::info("D3D12 INFO: {}", message);
        break;
    case D3D12_MESSAGE_SEVERITY_MESSAGE:
        spdlog::debug("D3D12 MESSAGE: {}", message);
        break;
    }
}
