#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"
#include "VirtualGeometry/Streaming/CLodStreamingTraceInternals.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

void CLodStreamingSystem::EnsureStreamingDiagnosticsCapacity(uint32_t requiredGroupCount) {
    if (requiredGroupCount > m_streamingDiagnosticsByGroup.size()) {
        m_streamingDiagnosticsByGroup.resize(requiredGroupCount);
    }
}

void CLodStreamingSystem::RecordStreamingRequestObserved(
    uint32_t groupIndex,
    uint32_t priority,
    uint64_t readbackDecodedNs) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag = {};
        diag.firstRequestTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.requestId = m_nextStreamingRequestTraceId++;
            diag.readbackDecodedNs = readbackDecodedNs != 0u
                ? readbackDecodedNs
                : CLodRequestTraceNowNs();
        }
        diag.active = true;
    }
    diag.priority = std::max(diag.priority, priority);
    diag.lastRequestTick = m_streamingDiagnosticTick;
}

void CLodStreamingSystem::RecordStreamingRequestQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.cpuQueuedTick == 0u) {
        diag.cpuQueuedTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.cpuQueuedNs = CLodRequestTraceNowNs();
        }
    }
}

void CLodStreamingSystem::RecordStreamingDuplicateRequest(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    ++diag.duplicateRequests;
    ++m_streamingDiagnosticsDuplicateRequestsThisFrame;
}

void CLodStreamingSystem::RecordStreamingDiskQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.diskQueuedTick == 0u) {
        diag.diskQueuedTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.diskQueuedNs = CLodRequestTraceNowNs();
        }
    }
}

void CLodStreamingSystem::RecordStreamingCompletion(
    uint32_t groupIndex,
    const br::render::CLodDiskStreamingCompletion& completion) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    diag.uploadedBytes = completion.totalStreamedBytes;
    if (diag.ioTaskQueuedNs == 0u) {
        diag.ioTaskQueuedNs = completion.ioTaskQueuedNs;
    }
    if (diag.ioTaskStartedNs == 0u) {
        diag.ioTaskStartedNs = completion.ioTaskStartedNs;
    }
    if (diag.ioTaskCompletedNs == 0u) {
        diag.ioTaskCompletedNs = completion.ioTaskCompletedNs;
    }
    const bool firstCompletion = diag.diskCompletedTick == 0u;
    if (firstCompletion) {
        diag.diskCompletedTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.diskCompletedNs = CLodRequestTraceNowNs();
        }
        if (completion.success) {
            ++m_streamingDiagnosticsCompletionSuccessThisFrame;
        } else {
            ++m_streamingDiagnosticsCompletionFailedThisFrame;
        }
    }
    if (diag.diskQueuedTick != 0u && firstCompletion) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(m_streamingDiagnosticTick - diag.diskQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame;
        m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame += ticks;
        m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame =
            std::max(m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame, ticks);
    }
}

void CLodStreamingSystem::RecordStreamingUploadQueued(uint32_t groupIndex, uint64_t bytes) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.uploadQueuedTick == 0u) {
        diag.uploadQueuedTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.uploadQueuedNs = CLodRequestTraceNowNs();
        }
        ++m_streamingDiagnosticsUploadQueuedGroupsThisFrame;
        if (diag.active) {
            const uint32_t ticks = static_cast<uint32_t>(
                std::min<uint64_t>(m_streamingDiagnosticTick - diag.firstRequestTick, UINT32_MAX));
            ++m_streamingDiagnosticsRequestToUploadSamplesThisFrame;
            m_streamingDiagnosticsRequestToUploadSumThisFrame += ticks;
            if (ticks > m_streamingDiagnosticsRequestToUploadWorstThisFrame) {
                m_streamingDiagnosticsRequestToUploadWorstThisFrame = ticks;
                m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame = groupIndex;
            }
        }
    }
    m_streamingDiagnosticsUploadQueuedBytesThisFrame += bytes;
}

void CLodStreamingSystem::RecordStreamingCommitQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.commitQueuedTick == 0u) {
        diag.commitQueuedTick = m_streamingDiagnosticTick;
        if (CLodRequestTraceEnabled()) {
            diag.commitQueuedNs = CLodRequestTraceNowNs();
        }
    }
}

void CLodStreamingSystem::RecordStreamingUploadSubmitted(
    uint32_t groupIndex) {
    if (!CLodRequestTraceEnabled() ||
        groupIndex >= m_streamingDiagnosticsByGroup.size()) {
        return;
    }
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (diag.active && diag.uploadSubmittedNs == 0u) {
        diag.uploadSubmittedNs = CLodRequestTraceNowNs();
    }
}

void CLodStreamingSystem::RecordStreamingPromoted(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        return;
    }

    diag.residentTick = m_streamingDiagnosticTick;
    if (CLodRequestTraceEnabled()) {
        diag.residentNs = CLodRequestTraceNowNs();
    }
    const uint32_t requestToResident = static_cast<uint32_t>(
        std::min<uint64_t>(diag.residentTick - diag.firstRequestTick, UINT32_MAX));
    ++m_streamingDiagnosticsRequestToResidentSamplesThisFrame;
    m_streamingDiagnosticsRequestToResidentSumThisFrame += requestToResident;
    if (requestToResident > m_streamingDiagnosticsRequestToResidentWorstThisFrame) {
        m_streamingDiagnosticsRequestToResidentWorstThisFrame = requestToResident;
        m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame = groupIndex;
    }
    if (diag.uploadQueuedTick != 0u) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(diag.residentTick - diag.uploadQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsUploadToResidentSamplesThisFrame;
        m_streamingDiagnosticsUploadToResidentSumThisFrame += ticks;
        m_streamingDiagnosticsUploadToResidentWorstThisFrame =
            std::max(m_streamingDiagnosticsUploadToResidentWorstThisFrame, ticks);
    }
    if (diag.commitQueuedTick != 0u) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(diag.residentTick - diag.commitQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsCommitToResidentSamplesThisFrame;
        m_streamingDiagnosticsCommitToResidentSumThisFrame += ticks;
        m_streamingDiagnosticsCommitToResidentWorstThisFrame =
            std::max(m_streamingDiagnosticsCommitToResidentWorstThisFrame, ticks);
    }

    CompleteStreamingRequestTrace(groupIndex, true);
    diag = {};
}

void CLodStreamingSystem::CompleteStreamingRequestTrace(
    uint32_t groupIndex,
    bool resident) {
    if (!CLodRequestTraceEnabled() ||
        groupIndex >= m_streamingDiagnosticsByGroup.size()) {
        return;
    }
    const auto& diagnostics =
        m_streamingDiagnosticsByGroup[groupIndex];
    if (!diagnostics.active || diagnostics.requestId == 0u) {
        return;
    }
    constexpr size_t maxCompletedTraces = 100000u;
    if (m_completedStreamingRequestTraces.size() >=
        maxCompletedTraces) {
        ++m_droppedStreamingRequestTraceCount;
        return;
    }
    m_completedStreamingRequestTraces.push_back(
        CompletedStreamingRequestTrace{
            groupIndex,
            resident,
            diagnostics});
}

void CLodStreamingSystem::RecordStreamingTerminal(uint32_t groupIndex) {
    if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
        CompleteStreamingRequestTrace(groupIndex, false);
        m_streamingDiagnosticsByGroup[groupIndex] = {};
    }
}

void CLodStreamingSystem::WriteStreamingRequestTraceReport() {
    const std::string& outputPath = CLodRequestTraceOutputPath();
    if (outputPath.empty()) {
        return;
    }

    const auto durationUs = [](uint64_t begin, uint64_t end) {
        return begin != 0u && end >= begin
            ? (end - begin) / 1000u
            : 0u;
    };
    const auto summarize = [](std::vector<uint64_t> values) {
        nlohmann::json result{
            {"count", values.size()},
            {"mean_us", 0.0},
            {"p50_us", 0u},
            {"p95_us", 0u},
            {"p99_us", 0u},
            {"max_us", 0u}};
        if (values.empty()) {
            return result;
        }
        const uint64_t total =
            std::accumulate(values.begin(), values.end(), uint64_t{0u});
        std::sort(values.begin(), values.end());
        const auto percentile = [&values](uint32_t percentileValue) {
            const size_t index = std::min<size_t>(
                values.size() - 1u,
                ((values.size() - 1u) * percentileValue + 99u) /
                    100u);
            return values[index];
        };
        result["mean_us"] =
            static_cast<double>(total) /
            static_cast<double>(values.size());
        result["p50_us"] = percentile(50u);
        result["p95_us"] = percentile(95u);
        result["p99_us"] = percentile(99u);
        result["max_us"] = values.back();
        return result;
    };

    std::vector<uint64_t> readbackToCpuQueue;
    std::vector<uint64_t> cpuQueueWait;
    std::vector<uint64_t> diskIo;
    std::vector<uint64_t> ioTaskQueueWait;
    std::vector<uint64_t> ioActiveRead;
    std::vector<uint64_t> ioResultWait;
    std::vector<uint64_t> completionToUpload;
    std::vector<uint64_t> completionToCommit;
    std::vector<uint64_t> uploadToCommit;
    std::vector<uint64_t> commitToSubmit;
    std::vector<uint64_t> submitToResident;
    std::vector<uint64_t> commitToResident;
    std::vector<uint64_t> requestToResident;
    std::vector<uint64_t> liveRequestToResident;
    std::vector<size_t> residentTraceIndices;
    uint64_t terminalCount = 0u;
    const auto appendDuration =
        [&durationUs](
            std::vector<uint64_t>& output,
            uint64_t begin,
            uint64_t end) {
            if (begin != 0u && end >= begin) {
                output.push_back(durationUs(begin, end));
            }
        };
    for (size_t index = 0u;
         index < m_completedStreamingRequestTraces.size();
         ++index) {
        const auto& trace = m_completedStreamingRequestTraces[index];
        const auto& diag = trace.diagnostics;
        if (!trace.resident) {
            ++terminalCount;
            continue;
        }
        residentTraceIndices.push_back(index);
        appendDuration(
            readbackToCpuQueue,
            diag.readbackDecodedNs,
            diag.cpuQueuedNs);
        appendDuration(
            cpuQueueWait,
            diag.cpuQueuedNs,
            diag.diskQueuedNs);
        appendDuration(
            diskIo,
            diag.diskQueuedNs,
            diag.diskCompletedNs);
        appendDuration(
            ioTaskQueueWait,
            diag.ioTaskQueuedNs,
            diag.ioTaskStartedNs);
        appendDuration(
            ioActiveRead,
            diag.ioTaskStartedNs,
            diag.ioTaskCompletedNs);
        appendDuration(
            ioResultWait,
            diag.ioTaskCompletedNs,
            diag.diskCompletedNs);
        appendDuration(
            completionToUpload,
            diag.diskCompletedNs,
            diag.uploadQueuedNs);
        appendDuration(
            completionToCommit,
            diag.diskCompletedNs,
            diag.commitQueuedNs);
        appendDuration(
            uploadToCommit,
            diag.uploadQueuedNs,
            diag.commitQueuedNs);
        appendDuration(
            commitToSubmit,
            diag.commitQueuedNs,
            diag.uploadSubmittedNs);
        appendDuration(
            submitToResident,
            diag.uploadSubmittedNs,
            diag.residentNs);
        appendDuration(
            commitToResident,
            diag.commitQueuedNs,
            diag.residentNs);
        appendDuration(
            requestToResident,
            diag.readbackDecodedNs,
            diag.residentNs);
        if (diag.liveAtAdmission) {
            appendDuration(
                liveRequestToResident,
                diag.readbackDecodedNs,
                diag.residentNs);
        }
    }
    std::sort(
        residentTraceIndices.begin(),
        residentTraceIndices.end(),
        [this, &durationUs](size_t lhs, size_t rhs) {
            const auto& lhsDiag =
                m_completedStreamingRequestTraces[lhs].diagnostics;
            const auto& rhsDiag =
                m_completedStreamingRequestTraces[rhs].diagnostics;
            return durationUs(
                       lhsDiag.readbackDecodedNs,
                       lhsDiag.residentNs) >
                durationUs(
                       rhsDiag.readbackDecodedNs,
                       rhsDiag.residentNs);
        });

    const auto makeTraceJson =
        [&durationUs](const CompletedStreamingRequestTrace& trace) {
            const auto& diag = trace.diagnostics;
            const uint64_t base = diag.readbackDecodedNs;
            const auto offsetUs = [base](uint64_t timestamp) {
                return timestamp != 0u && timestamp >= base
                    ? (timestamp - base) / 1000u
                    : 0u;
            };
            const std::array<std::pair<const char*, uint64_t>, 12u>
                stages{{
                    {"readback_to_cpu_queue",
                     durationUs(
                         diag.readbackDecodedNs,
                         diag.cpuQueuedNs)},
                    {"cpu_queue_wait",
                     durationUs(diag.cpuQueuedNs, diag.diskQueuedNs)},
                    {"disk_io",
                     durationUs(diag.diskQueuedNs, diag.diskCompletedNs)},
                    {"io_task_queue",
                     durationUs(
                         diag.ioTaskQueuedNs,
                         diag.ioTaskStartedNs)},
                    {"io_active_read",
                     durationUs(
                         diag.ioTaskStartedNs,
                         diag.ioTaskCompletedNs)},
                    {"io_result_wait",
                     durationUs(
                         diag.ioTaskCompletedNs,
                         diag.diskCompletedNs)},
                    {"completion_to_upload",
                     durationUs(
                         diag.diskCompletedNs,
                         diag.uploadQueuedNs)},
                    {"completion_to_commit",
                     durationUs(
                         diag.diskCompletedNs,
                         diag.commitQueuedNs)},
                    {"upload_to_commit",
                     durationUs(
                         diag.uploadQueuedNs,
                         diag.commitQueuedNs)},
                    {"commit_to_submit",
                     durationUs(
                         diag.commitQueuedNs,
                         diag.uploadSubmittedNs)},
                    {"submit_to_resident",
                     durationUs(
                         diag.uploadSubmittedNs,
                         diag.residentNs)},
                    {"commit_to_resident",
                     durationUs(
                         diag.commitQueuedNs,
                         diag.residentNs)},
                }};
            const auto worstStage = std::max_element(
                stages.begin(),
                stages.end(),
                [](const auto& lhs, const auto& rhs) {
                    return lhs.second < rhs.second;
                });
            return nlohmann::json{
                {"request_id", diag.requestId},
                {"group_index", trace.groupIndex},
                {"outcome", trace.resident ? "resident" : "terminal"},
                {"priority", diag.priority},
                {"uploaded_bytes", diag.uploadedBytes},
                {"duplicate_requests", diag.duplicateRequests},
                {"preallocation_deferrals",
                 diag.preallocationDeferrals},
                {"promotion_deferrals", diag.promotionDeferrals},
                {"live_at_admission", diag.liveAtAdmission},
                {"total_us",
                 durationUs(
                     diag.readbackDecodedNs,
                     trace.resident ? diag.residentNs
                                    : std::max(
                                          diag.diskCompletedNs,
                                          diag.commitQueuedNs))},
                {"worst_stage", worstStage->first},
                {"worst_stage_us", worstStage->second},
                {"stage_offsets_us",
                 {
                     {"readback_decoded", 0u},
                     {"cpu_queued", offsetUs(diag.cpuQueuedNs)},
                      {"disk_queued", offsetUs(diag.diskQueuedNs)},
                      {"io_task_started", offsetUs(diag.ioTaskStartedNs)},
                      {"io_task_queued", offsetUs(diag.ioTaskQueuedNs)},
                      {"io_task_completed", offsetUs(diag.ioTaskCompletedNs)},
                     {"disk_completed",
                      offsetUs(diag.diskCompletedNs)},
                     {"upload_queued", offsetUs(diag.uploadQueuedNs)},
                     {"commit_queued", offsetUs(diag.commitQueuedNs)},
                     {"upload_submitted",
                      offsetUs(diag.uploadSubmittedNs)},
                     {"resident", offsetUs(diag.residentNs)},
                 }},
                {"stage_durations_us",
                 {
                     {"readback_to_cpu_queue",
                      durationUs(
                          diag.readbackDecodedNs,
                          diag.cpuQueuedNs)},
                     {"cpu_queue_wait",
                      durationUs(
                          diag.cpuQueuedNs,
                          diag.diskQueuedNs)},
                      {"disk_io",
                      durationUs(
                          diag.diskQueuedNs,
                           diag.diskCompletedNs)},
                      {"io_task_queue",
                       durationUs(
                           diag.ioTaskQueuedNs,
                           diag.ioTaskStartedNs)},
                      {"io_active_read",
                       durationUs(
                           diag.ioTaskStartedNs,
                           diag.ioTaskCompletedNs)},
                      {"io_result_wait",
                       durationUs(
                           diag.ioTaskCompletedNs,
                           diag.diskCompletedNs)},
                     {"completion_to_commit",
                      durationUs(
                          diag.diskCompletedNs,
                          diag.commitQueuedNs)},
                     {"commit_to_submit",
                      durationUs(
                          diag.commitQueuedNs,
                          diag.uploadSubmittedNs)},
                     {"submit_to_resident",
                      durationUs(
                          diag.uploadSubmittedNs,
                          diag.residentNs)},
                 }},
            };
        };

    nlohmann::json worstRequests = nlohmann::json::array();
    const size_t worstCount =
        std::min<size_t>(residentTraceIndices.size(), 100u);
    for (size_t rank = 0u; rank < worstCount; ++rank) {
        worstRequests.push_back(makeTraceJson(
            m_completedStreamingRequestTraces[
                residentTraceIndices[rank]]));
    }
    nlohmann::json traces = nlohmann::json::array();
    for (const auto& trace : m_completedStreamingRequestTraces) {
        traces.push_back(makeTraceJson(trace));
    }

    uint64_t activeCount = 0u;
    const uint64_t reportNowNs = CLodRequestTraceNowNs();
    nlohmann::json oldestActive = nlohmann::json::array();
    std::vector<std::pair<uint64_t, uint32_t>> activeByAge;
    for (uint32_t groupIndex = 0u;
         groupIndex < m_streamingDiagnosticsByGroup.size();
         ++groupIndex) {
        const auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
        if (!diag.active || diag.requestId == 0u) {
            continue;
        }
        ++activeCount;
        activeByAge.emplace_back(
            durationUs(diag.readbackDecodedNs, reportNowNs),
            groupIndex);
    }
    std::sort(activeByAge.begin(), activeByAge.end(), std::greater{});
    for (size_t index = 0u;
         index < std::min<size_t>(activeByAge.size(), 100u);
         ++index) {
        const auto [ageUs, groupIndex] = activeByAge[index];
        const auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
        const char* currentStage = "readback";
        uint64_t currentStageStartNs = diag.readbackDecodedNs;
        if (diag.uploadSubmittedNs != 0u) {
            currentStage = "upload_submitted";
            currentStageStartNs = diag.uploadSubmittedNs;
        } else if (diag.commitQueuedNs != 0u) {
            currentStage = "commit_waiting_submission";
            currentStageStartNs = diag.commitQueuedNs;
        } else if (diag.diskCompletedNs != 0u) {
            currentStage = "completion_processing";
            currentStageStartNs = diag.diskCompletedNs;
        } else if (diag.diskQueuedNs != 0u) {
            currentStage = "disk_io";
            currentStageStartNs = diag.diskQueuedNs;
        } else if (diag.cpuQueuedNs != 0u) {
            currentStage = "cpu_queue";
            currentStageStartNs = diag.cpuQueuedNs;
        }
        oldestActive.push_back({
            {"request_id", diag.requestId},
            {"group_index", groupIndex},
            {"age_us", ageUs},
            {"current_stage", currentStage},
            {"current_stage_age_us",
             durationUs(currentStageStartNs, reportNowNs)},
            {"priority", diag.priority},
            {"duplicate_requests", diag.duplicateRequests},
            {"preallocation_deferrals", diag.preallocationDeferrals},
            {"promotion_deferrals", diag.promotionDeferrals},
        });
    }

    uint64_t traceStartNs = reportNowNs;
    uint64_t residentBytes = 0u;
    for (size_t index : residentTraceIndices) {
        const auto& diag =
            m_completedStreamingRequestTraces[index].diagnostics;
        if (diag.readbackDecodedNs != 0u) {
            traceStartNs = std::min(traceStartNs, diag.readbackDecodedNs);
        }
        residentBytes += diag.uploadedBytes;
    }
    for (const auto& diag : m_streamingDiagnosticsByGroup) {
        if (diag.active && diag.readbackDecodedNs != 0u) {
            traceStartNs =
                std::min(traceStartNs, diag.readbackDecodedNs);
        }
    }
    const double measuredSeconds =
        reportNowNs > traceStartNs
        ? static_cast<double>(reportNowNs - traceStartNs) / 1.0e9
        : 0.0;
    size_t sharedPageWaiterCount = 0u;
    for (const auto& waiters :
         m_readyStreamingCompletionWaitersByPage) {
        sharedPageWaiterCount += waiters.size();
    }
    size_t parentResidencyWaiterCount = 0u;
    for (uint32_t groupIndex = 0u;
         groupIndex < m_readyStreamingCompletionWaitParentByGroup.size();
         ++groupIndex) {
        parentResidencyWaiterCount +=
            m_readyStreamingCompletionWaitParentByGroup[groupIndex] !=
                    UINT32_MAX &&
                m_readyStreamingCompletionsByGroup.contains(groupIndex)
            ? 1u
            : 0u;
    }

    nlohmann::json report{
        {"schema_version", 2u},
        {"clock", "steady_clock_nanoseconds"},
        {"counts",
         {
             {"resident", residentTraceIndices.size()},
             {"terminal", terminalCount},
             {"active_at_shutdown", activeCount},
             {"dropped", m_droppedStreamingRequestTraceCount},
         }},
        {"summary_us",
         {
             {"readback_to_cpu_queue", summarize(readbackToCpuQueue)},
             {"cpu_queue_wait", summarize(cpuQueueWait)},
             {"disk_io", summarize(diskIo)},
              {"io_task_queue", summarize(ioTaskQueueWait)},
              {"io_active_read", summarize(ioActiveRead)},
              {"io_result_wait", summarize(ioResultWait)},
             {"completion_to_upload",
              summarize(completionToUpload)},
             {"completion_to_commit",
              summarize(completionToCommit)},
             {"upload_to_commit", summarize(uploadToCommit)},
             {"commit_to_submit", summarize(commitToSubmit)},
             {"submit_to_resident", summarize(submitToResident)},
             {"commit_to_resident", summarize(commitToResident)},
              {"request_to_resident", summarize(requestToResident)},
              {"live_request_to_resident",
               summarize(liveRequestToResident)},
         }},
        {"throughput",
         {
             {"measured_seconds", measuredSeconds},
             {"resident_requests_per_second",
              measuredSeconds > 0.0
                  ? static_cast<double>(
                        residentTraceIndices.size()) /
                        measuredSeconds
                  : 0.0},
             {"resident_mib_per_second",
              measuredSeconds > 0.0
                  ? static_cast<double>(residentBytes) /
                        (1024.0 * 1024.0 * measuredSeconds)
                  : 0.0},
         }},
        {"scheduler",
         {
             {"io_admission_depth", m_streamingIoAdmissionDepth},
             {"io_worker_count", m_streamingIoWorkerCount},
             {"io_task_batch_size", m_streamingIoTaskBatchSize},
             {"staged_payload_group_limit",
              CLodStagedPayloadGroupLimit()},
             {"staged_payload_groups",
              m_readyStreamingCompletionsByGroup.size()},
             {"staged_payload_bytes",
              m_readyStreamingCompletionBytes},
             {"peak_staged_payload_groups",
              m_peakReadyStreamingCompletionCount},
             {"peak_staged_payload_bytes",
              m_peakReadyStreamingCompletionBytes},
             {"page_credit_waiters",
              m_readyStreamingCompletionPageCreditWaitGroups.size() -
                  std::min(
                      m_readyStreamingCompletionPageCreditWaitCursor,
                      m_readyStreamingCompletionPageCreditWaitGroups.size())},
             {"page_credit_retry_budget",
              CLodPageCreditRetryBudget()},
             {"ready_completions",
              m_readyStreamingCompletionsByGroup.size()},
             {"parent_residency_waiters",
              parentResidencyWaiterCount},
             {"transactional_child_completion_admissions",
              m_transactionalChildCompletionAdmissions},
             {"transactional_child_promotions",
              m_transactionalChildPromotions},
             {"shared_page_waiters", sharedPageWaiterCount},
         }},
        {"worst_resident_requests", std::move(worstRequests)},
        {"oldest_active_requests", std::move(oldestActive)},
        {"requests", std::move(traces)}};

    const std::filesystem::path path(outputPath);
    std::error_code directoryError;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(
            path.parent_path(),
            directoryError);
    }
    std::ofstream output(path, std::ios::trunc);
    if (!output) {
        spdlog::error(
            "CLOD request trace: failed to open report '{}'",
            outputPath);
        return;
    }
    output << report.dump(2) << '\n';
    spdlog::info(
        "CLOD request trace: wrote {} completed requests to '{}'",
        m_completedStreamingRequestTraces.size(),
        outputPath);
}

