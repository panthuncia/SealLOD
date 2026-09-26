#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"

#include <algorithm>
#include <utility>
#include <vector>

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

