#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingTraceInternals.h"

#include <algorithm>
#include <vector>

#include <BasicTelemetry/Telemetry.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

void CLodStreamingSystem::PollCompletedReadbackSlots() {
    ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots");

    ++m_streamingDiagnosticTick;

    // Drain decoded (groupIndex, priority) pairs produced by the scheduler drain.
    m_readbackBatchScratch.clear();
    m_usedGroupsBatchScratch.clear();
    {
        ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots::ConsumeWorkerBatches");
        m_readbackBatchScratch.swap(m_decodedReadbackBatch);
        m_usedGroupsBatchScratch.swap(m_decodedUsedGroupsBatch);
        if (m_usedGroupsCpuSampleGeneration != m_decodedUsedGroupsSampleGeneration) {
            ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots::RebuildUsedGroupsBitset");
            m_usedGroupsCpuSampleGeneration = m_decodedUsedGroupsSampleGeneration;
            for (uint32_t word : m_usedGroupsWordsCpu) {
                if (word < m_usedGroupsBitsCpu.size()) {
                    m_usedGroupsBitsCpu[word] = 0u;
                }
            }
            m_usedGroupsWordsCpu.clear();
            for (const uint32_t groupIndex : m_usedGroupsBatchScratch) {
                const uint32_t wa = BitWordAddress(groupIndex);
                if (wa < m_usedGroupsBitsCpu.size()) {
                    if (m_usedGroupsBitsCpu[wa] == 0u) {
                        m_usedGroupsWordsCpu.push_back(wa);
                    }
                    m_usedGroupsBitsCpu[wa] |= BitMask(groupIndex);
                }
                if (groupIndex < m_groupLastUsedTick.size()) {
                    m_groupLastUsedTick[groupIndex] = m_streamingDiagnosticTick;
                    if (!m_recentlyUsedGroupTrackedCpu[groupIndex]) {
                        m_recentlyUsedGroupTrackedCpu[groupIndex] = 1u;
                        m_recentlyUsedGroupsCpu.push_back(groupIndex);
                    }
                }
            }
        }
    }

    // ProtectReferencedPages::UsedGroups immediately follows this poll in the
    // streaming service. It already walks the same selected-parent chains and
    // touches their pages, so doing that here as well only duplicates the work.
    {
        ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots::TouchVisibleGroupsLru");
        const bool recordCpuTiming = basic_telemetry::Enabled();
        const uint64_t timingStartNs =
            recordCpuTiming ? basic_telemetry::NowNs() : 0u;
        if (recordCpuTiming) {
            basic_telemetry::Record(
                "CLod.Bookkeeping.TouchVisibleGroupsLRU",
                basic_telemetry::NowNs() - timingStartNs);
        }
    }

    if (m_readbackBatchScratch.empty()) {
        return;
    }

    m_streamingDiagnosticsDecodedRequestsThisFrame += static_cast<uint32_t>(m_readbackBatchScratch.size());
    uint32_t queuedCount = 0;
    {
        ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots::QueueLoadRequests");
        {
            ZoneScopedN("CLodStreamingWorker::QueueLoadRequests");
            for (const auto& decoded : m_readbackBatchScratch) {
                CLodStreamingRequest req{};
                req.groupGlobalIndex = decoded.groupIndex;
                queuedCount += QueueLoadRequestWithParents(
                    req,
                    decoded.priority,
                    decoded.decodedNs);
            }
        }
    }
    m_streamingDiagnosticsQueuedLoadRequestsThisFrame += queuedCount;

    spdlog::debug(
        "CLod streaming: drained {} decoded groups from worker, {} queued, {} LRU touches",
        static_cast<uint32_t>(m_readbackBatchScratch.size()),
        queuedCount,
        static_cast<uint32_t>(m_usedGroupsBatchScratch.size()));
}

