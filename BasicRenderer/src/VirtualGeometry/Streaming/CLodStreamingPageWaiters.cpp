#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

void CLodStreamingSystem::ParkReadyCompletionForSharedPage(
    uint32_t groupIndex,
    uint32_t page,
    uint64_t key,
    br::render::CLodDiskStreamingCompletion&& completion) {
    StoreReadyStreamingCompletion(groupIndex, std::move(completion));
    if (groupIndex >= m_readyStreamingCompletionWaitPageByGroup.size() ||
        page >= m_readyStreamingCompletionWaitersByPage.size()) {
        if (groupIndex < m_readyStreamingCompletionRetryQueuedByGroup.size() &&
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] == 0u) {
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 1u;
            m_readyStreamingCompletionRetryGroups.push_back(groupIndex);
        }
        return;
    }

    m_readyStreamingCompletionWaitPageByGroup[groupIndex] = page;
    m_readyStreamingCompletionWaitKeyByGroup[groupIndex] = key;
    m_readyStreamingCompletionWaitGenerationByGroup[groupIndex] =
        groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
        ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
        : 0u;
    m_readyStreamingCompletionWaitersByPage[page].push_back(groupIndex);
}

void CLodStreamingSystem::StoreReadyStreamingCompletion(
    uint32_t groupIndex,
    br::render::CLodDiskStreamingCompletion&& completion) {
    auto existing = m_readyStreamingCompletionsByGroup.find(groupIndex);
    if (existing != m_readyStreamingCompletionsByGroup.end()) {
        m_readyStreamingCompletionBytes -=
            std::min(
                m_readyStreamingCompletionBytes,
                CLodReadyCompletionStorageBytes(existing->second));
        existing->second = std::move(completion);
    }
    else {
        m_readyStreamingCompletionsByGroup.emplace(
            groupIndex, std::move(completion));
    }
    const auto stored = m_readyStreamingCompletionsByGroup.find(groupIndex);
    if (stored != m_readyStreamingCompletionsByGroup.end()) {
        m_readyStreamingCompletionBytes +=
            CLodReadyCompletionStorageBytes(stored->second);
    }
    m_peakReadyStreamingCompletionBytes = std::max(
        m_peakReadyStreamingCompletionBytes,
        m_readyStreamingCompletionBytes);
    m_peakReadyStreamingCompletionCount = std::max<uint32_t>(
        m_peakReadyStreamingCompletionCount,
        static_cast<uint32_t>(
            m_readyStreamingCompletionsByGroup.size()));
}

void CLodStreamingSystem::ParkReadyCompletionForPageCredit(
    uint32_t groupIndex,
    br::render::CLodDiskStreamingCompletion&& completion) {
    StoreReadyStreamingCompletion(groupIndex, std::move(completion));
    if (groupIndex >=
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup.size()) {
        return;
    }
    if (m_readyStreamingCompletionPageCreditWaitQueuedByGroup[groupIndex] ==
        0u) {
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup[groupIndex] =
            1u;
        m_readyStreamingCompletionPageCreditWaitGroups.push_back(groupIndex);
    }
}

void CLodStreamingSystem::ParkReadyCompletionForParent(
    uint32_t groupIndex,
    uint32_t parentGroupIndex,
    br::render::CLodDiskStreamingCompletion&& completion) {
    StoreReadyStreamingCompletion(groupIndex, std::move(completion));
    if (groupIndex >= m_readyStreamingCompletionWaitParentByGroup.size()) {
        return;
    }

    const uint32_t previousParent =
        m_readyStreamingCompletionWaitParentByGroup[groupIndex];
    if (previousParent == parentGroupIndex) {
        return;
    }
    m_readyStreamingCompletionWaitParentByGroup[groupIndex] =
        parentGroupIndex;
    m_readyStreamingCompletionWaitParentGenerationByGroup[groupIndex] =
        groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
            ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
            : 0u;
    m_readyStreamingCompletionWaitersByParent[parentGroupIndex].push_back(
        groupIndex);
}

void CLodStreamingSystem::WakeReadyPageCreditWaiters(
    uint32_t availablePageCredits) {
    while (availablePageCredits != 0u &&
        m_readyStreamingCompletionPageCreditWaitCursor <
            m_readyStreamingCompletionPageCreditWaitGroups.size()) {
        const uint32_t groupIndex =
            m_readyStreamingCompletionPageCreditWaitGroups[
                m_readyStreamingCompletionPageCreditWaitCursor++];
        if (groupIndex >=
            m_readyStreamingCompletionPageCreditWaitQueuedByGroup.size()) {
            continue;
        }
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup[groupIndex] =
            0u;
        if (m_readyStreamingCompletionsByGroup.find(groupIndex) ==
            m_readyStreamingCompletionsByGroup.end()) {
            continue;
        }
        if (groupIndex <
                m_readyStreamingCompletionRetryQueuedByGroup.size() &&
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] == 0u) {
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 1u;
            m_readyStreamingCompletionRetryGroups.push_back(groupIndex);
            --availablePageCredits;
        }
    }
    if (m_readyStreamingCompletionPageCreditWaitCursor ==
        m_readyStreamingCompletionPageCreditWaitGroups.size()) {
        m_readyStreamingCompletionPageCreditWaitGroups.clear();
        m_readyStreamingCompletionPageCreditWaitCursor = 0u;
    }
}

void CLodStreamingSystem::PruneStaleReadyStreamingCompletions(
    uint32_t maxCompletions) {
    if (maxCompletions == 0u ||
        m_readyStreamingCompletionsByGroup.empty()) {
        return;
    }
    const uint64_t liveWindow = static_cast<uint64_t>(
        m_streamingReadbackRingSize + 2u);
    m_staleReadyCompletionGroupsScratch.clear();
    m_staleReadyCompletionGroupsScratch.reserve(
        std::min<size_t>(
            maxCompletions,
            m_readyStreamingCompletionsByGroup.size()));
    for (const auto& [groupIndex, _] :
        m_readyStreamingCompletionsByGroup) {
        if (m_staleReadyCompletionGroupsScratch.size() >= maxCompletions) {
            break;
        }
        if (IsGroupPinned(groupIndex) ||
            groupIndex >= m_streamingDiagnosticsByGroup.size()) {
            continue;
        }
        const uint64_t lastRequestTick =
            m_streamingDiagnosticsByGroup[groupIndex].lastRequestTick;
        if (lastRequestTick != 0u &&
            m_streamingDiagnosticTick <= lastRequestTick + liveWindow) {
            continue;
        }
        m_staleReadyCompletionGroupsScratch.push_back(groupIndex);
    }

    for (uint32_t groupIndex : m_staleReadyCompletionGroupsScratch) {
        if (m_readyStreamingCompletionsByGroup.find(groupIndex) ==
            m_readyStreamingCompletionsByGroup.end()) {
            continue;
        }
        ClearStreamingRequestInProgress(groupIndex);
        ClearPendingLoadPriority(groupIndex);
    }
}

void CLodStreamingSystem::LogStuckReadyStreamingCompletions() {
    // Parked completions normally leave within a few service ticks. If residency stops moving while
    // some remain and nothing else is in flight, a wake was lost, and this says which wait holds them.
    constexpr uint64_t kStuckTicks = 600u;
    const uint32_t otherWork = m_pendingStreamingRequestCount + m_waitingForPagesRequestCount +
        static_cast<uint32_t>(m_pendingResidencyCommitGroups.size());
    const uint32_t residentCount = static_cast<uint32_t>(m_groupOwnedPages.size());
    if (m_readyStreamingCompletionsByGroup.empty() || otherWork != 0u ||
        residentCount != m_lastReadyCompletionResidentCount) {
        m_lastReadyCompletionProgressTick = m_streamingDiagnosticTick;
        m_lastReadyCompletionResidentCount = residentCount;
        m_stuckReadyCompletionsLogged = false;
        return;
    }
    if (m_stuckReadyCompletionsLogged || m_streamingDiagnosticTick < m_lastReadyCompletionProgressTick + kStuckTicks) {
        return;
    }
    m_stuckReadyCompletionsLogged = true;
    auto* meshManager = m_geometryStorage;
    for (const auto& [groupIndex, completion] : m_readyStreamingCompletionsByGroup) {
        uint32_t parent = UINT32_MAX;
        const bool hasParent = meshManager != nullptr && meshManager->TryGetCLodParentGroup(groupIndex, parent);
        const auto at = [groupIndex](const auto& values, auto fallback) {
            return groupIndex < values.size() ? static_cast<uint64_t>(values[groupIndex]) : static_cast<uint64_t>(fallback);
        };
        spdlog::warn(
            "CLod streaming: ready completion stuck: group={} success={} request_state={} parent={} parent_resident={} parent_state={} parent_owned={} parent_commit={} parent_active={} wait_parent={} wait_page={} retry_queued={} credit_queued={} preallocated={} pinned={}",
            groupIndex,
            completion.success,
            at(m_streamingRequestStateByGroup, 0u),
            hasParent ? static_cast<int64_t>(parent) : -1,
            hasParent ? IsGroupResident(parent) : true,
            hasParent && parent < m_streamingRequestStateByGroup.size() ? static_cast<int>(m_streamingRequestStateByGroup[parent]) : -1,
            hasParent ? m_groupOwnedPages.contains(parent) : false,
            hasParent ? m_pendingResidencyCommitGroups.contains(parent) : false,
            hasParent ? IsGroupActive(parent) : false,
            at(m_readyStreamingCompletionWaitParentByGroup, UINT32_MAX),
            at(m_readyStreamingCompletionWaitPageByGroup, UINT32_MAX),
            at(m_readyStreamingCompletionRetryQueuedByGroup, 0u),
            at(m_readyStreamingCompletionPageCreditWaitQueuedByGroup, 0u),
            m_preAllocatedPagesByGroup.contains(groupIndex),
            IsGroupPinned(groupIndex));
    }
}

void CLodStreamingSystem::WakeReadyCompletionsForPage(
    uint32_t page,
    uint64_t key) {
    if (page >= m_readyStreamingCompletionWaitersByPage.size()) {
        return;
    }
    auto waiters = std::move(
        m_readyStreamingCompletionWaitersByPage[page]);
    m_readyStreamingCompletionWaitersByPage[page].clear();
    for (uint32_t groupIndex : waiters) {
        if (groupIndex >= m_readyStreamingCompletionWaitPageByGroup.size() ||
            m_readyStreamingCompletionWaitPageByGroup[groupIndex] != page ||
            m_readyStreamingCompletionWaitKeyByGroup[groupIndex] != key ||
            groupIndex >= m_pendingStreamingRequestGenerationByGroup.size() ||
            m_readyStreamingCompletionWaitGenerationByGroup[groupIndex] !=
                m_pendingStreamingRequestGenerationByGroup[groupIndex] ||
            m_readyStreamingCompletionsByGroup.find(groupIndex) ==
                m_readyStreamingCompletionsByGroup.end()) {
            continue;
        }
        m_readyStreamingCompletionWaitPageByGroup[groupIndex] = UINT32_MAX;
        m_readyStreamingCompletionWaitKeyByGroup[groupIndex] =
            kInvalidCLodMeshPageKey;
        if (m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] == 0u) {
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 1u;
            m_readyStreamingCompletionRetryGroups.push_back(groupIndex);
        }
    }
}

void CLodStreamingSystem::WakeReadyCompletionsForParent(
    uint32_t parentGroupIndex,
    std::vector<br::render::CLodDiskStreamingCompletion>*
        immediateCompletions) {
    auto waitersIt =
        m_readyStreamingCompletionWaitersByParent.find(parentGroupIndex);
    if (waitersIt == m_readyStreamingCompletionWaitersByParent.end()) {
        return;
    }

    auto waiters = std::move(waitersIt->second);
    m_readyStreamingCompletionWaitersByParent.erase(waitersIt);
    for (uint32_t groupIndex : waiters) {
        if (groupIndex >= m_readyStreamingCompletionWaitParentByGroup.size() ||
            m_readyStreamingCompletionWaitParentByGroup[groupIndex] !=
                parentGroupIndex ||
            groupIndex >= m_pendingStreamingRequestGenerationByGroup.size() ||
            m_readyStreamingCompletionWaitParentGenerationByGroup[groupIndex] !=
                m_pendingStreamingRequestGenerationByGroup[groupIndex] ||
            m_readyStreamingCompletionsByGroup.find(groupIndex) ==
                m_readyStreamingCompletionsByGroup.end()) {
            continue;
        }

        m_readyStreamingCompletionWaitParentByGroup[groupIndex] = UINT32_MAX;
        if (immediateCompletions != nullptr) {
            auto readyIt =
                m_readyStreamingCompletionsByGroup.find(groupIndex);
            if (readyIt == m_readyStreamingCompletionsByGroup.end()) {
                continue;
            }
            immediateCompletions->push_back(std::move(readyIt->second));
            m_readyStreamingCompletionBytes -=
                std::min(
                    m_readyStreamingCompletionBytes,
                    CLodReadyCompletionStorageBytes(
                        immediateCompletions->back()));
            m_readyStreamingCompletionsByGroup.erase(readyIt);
            continue;
        }
        if (m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] == 0u) {
            m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 1u;
            m_readyStreamingCompletionRetryGroups.push_back(groupIndex);
        }
    }
}

