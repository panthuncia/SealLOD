#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"

#include <algorithm>
#include <utility>
#include <tracy/Tracy.hpp>

bool CLodStreamingSystem::TryQueuePendingLoadRequest(
    const CLodStreamingRequest& req,
    uint32_t priority,
    uint64_t readbackDecodedNs) {
    const uint32_t groupIndex = req.groupGlobalIndex;
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    RecordStreamingRequestObserved(
        groupIndex,
        priority,
        readbackDecodedNs);

    if (IsGroupResident(groupIndex)) {
        RecordStreamingTerminal(groupIndex);
        return false;
    }

    if (IsStreamingRequestInProgress(groupIndex)) {
        RecordStreamingDuplicateRequest(groupIndex);
        // Update priority without enqueueing a duplicate request.
        const uint32_t oldPriority = GetPendingLoadPriority(groupIndex);
        uint32_t newPriority = oldPriority;
        if (m_priorityMode == CLodPriorityMode::Sum) {
            newPriority += priority;
        } else {
            newPriority = std::max(newPriority, priority);
        }
        if (newPriority != oldPriority) {
            if (groupIndex < m_streamingRequestStateByGroup.size()
                && m_streamingRequestStateByGroup[groupIndex] == StreamingRequestState::PendingCpu) {
                CLodStreamingRequest pendingReq = req;
                pendingReq.groupGlobalIndex = groupIndex;
                PushOrUpdatePendingStreamingRequest(pendingReq, newPriority);
            } else if (groupIndex < m_streamingRequestStateByGroup.size()
                && m_streamingRequestStateByGroup[groupIndex] == StreamingRequestState::WaitingForPages) {
                PendingStreamingRequest pending{};
                pending.request = req;
                pending.request.groupGlobalIndex = groupIndex;
                pending.priority = newPriority;
                pending.generation = groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
                    ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
                    : 0u;
                ParkStreamingRequestWaitingForPages(pending);
            } else {
                SetPendingLoadPriority(groupIndex, newPriority);
            }
        }

        return false;
    }

    PushOrUpdatePendingStreamingRequest(req, priority);
    RecordStreamingRequestQueued(groupIndex);
    return true;
}

uint32_t CLodStreamingSystem::QueueLoadRequestWithParents(
    const CLodStreamingRequest& requestedLoad,
    uint32_t requestedPriority,
    uint64_t readbackDecodedNs) {
    ZoneScopedN("CLodStreamingSystem::QueueLoadRequestWithParents");

    if (requestedLoad.groupGlobalIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(requestedLoad.groupGlobalIndex + 1u);
    }

    uint32_t queuedCount = 0u;
    m_parentChainScratch.clear();
    uint32_t currentGroup = requestedLoad.groupGlobalIndex;
    const size_t maxHops = m_streamingStorageGroupCapacity;
    for (size_t hop = 0; hop < maxHops; ++hop) {
        uint32_t parentGroup = 0;
        if (!TryGetCachedParentGroup(currentGroup, parentGroup) ||
            parentGroup == currentGroup) {
            break;
        }
        m_parentChainScratch.push_back(parentGroup);
        currentGroup = parentGroup;
    }

    for (auto it = m_parentChainScratch.rbegin(); it != m_parentChainScratch.rend(); ++it) {
        const uint32_t parentGroup = *it;

        if (IsGroupResident(parentGroup)) {
            continue;
        }

        CLodStreamingRequest parentLoad = requestedLoad;
        parentLoad.groupGlobalIndex = parentGroup;
        const uint32_t parentPriority =
            (requestedPriority == std::numeric_limits<uint32_t>::max())
                ? requestedPriority
                : requestedPriority + 1u;
        if (TryQueuePendingLoadRequest(
                parentLoad,
                parentPriority,
                readbackDecodedNs)) {
            queuedCount++;
        }
    }

    if (TryQueuePendingLoadRequest(
            requestedLoad,
            requestedPriority,
            readbackDecodedNs)) {
        queuedCount++;
    }

    return queuedCount;
}

bool CLodStreamingSystem::IsStreamingRequestInProgress(uint32_t groupIndex) const {
    return groupIndex < m_streamingRequestStateByGroup.size()
        && m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::None;
}

void CLodStreamingSystem::MarkStreamingRequestPending(uint32_t groupIndex) {
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }

    auto& state = m_streamingRequestStateByGroup[groupIndex];
    if (state == StreamingRequestState::None) {
        ++m_streamingRequestsInProgressCount;
    }
    if (state != StreamingRequestState::PendingCpu) {
        ++m_pendingStreamingRequestCount;
    }
    if (state == StreamingRequestState::WaitingForPages && m_waitingForPagesRequestCount > 0u) {
        --m_waitingForPagesRequestCount;
    }
    state = StreamingRequestState::PendingCpu;
}

void CLodStreamingSystem::MarkStreamingRequestWaitingForPages(uint32_t groupIndex) {
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }

    auto& state = m_streamingRequestStateByGroup[groupIndex];
    if (state == StreamingRequestState::None) {
        ++m_streamingRequestsInProgressCount;
    }
    if (state == StreamingRequestState::PendingCpu && m_pendingStreamingRequestCount > 0u) {
        --m_pendingStreamingRequestCount;
    }
    if (state != StreamingRequestState::WaitingForPages) {
        ++m_waitingForPagesRequestCount;
    }
    state = StreamingRequestState::WaitingForPages;
}

void CLodStreamingSystem::MarkStreamingRequestDiskIo(uint32_t groupIndex) {
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }

    auto& state = m_streamingRequestStateByGroup[groupIndex];
    if (state == StreamingRequestState::None) {
        ++m_streamingRequestsInProgressCount;
    }
    if (state == StreamingRequestState::PendingCpu && m_pendingStreamingRequestCount > 0u) {
        --m_pendingStreamingRequestCount;
    }
    if (state == StreamingRequestState::WaitingForPages && m_waitingForPagesRequestCount > 0u) {
        --m_waitingForPagesRequestCount;
    }
    state = StreamingRequestState::DiskIo;
    RecordStreamingDiskQueued(groupIndex);
}

void CLodStreamingSystem::ClearStreamingRequestInProgress(uint32_t groupIndex) {
    ZoneScopedN("CLodStreamingSystem::ClearStreamingRequestInProgress");

    if (groupIndex >= m_streamingRequestStateByGroup.size()) {
        return;
    }

    auto& state = m_streamingRequestStateByGroup[groupIndex];
    if (state == StreamingRequestState::None) {
        return;
    }

    if (state == StreamingRequestState::PendingCpu
        && groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()) {
        ZoneScopedN("CLodStreamingSystem::ClearStreamingRequestInProgress::RemovePendingHeapEntry");
        const uint32_t heapIndex = m_pendingStreamingRequestHeapIndexByGroup[groupIndex];
        if (heapIndex != UINT32_MAX
            && heapIndex < m_pendingStreamingRequests.size()
            && m_pendingStreamingRequests[heapIndex].request.groupGlobalIndex == groupIndex) {
            auto higherPriority = [this](uint32_t lhsIndex, uint32_t rhsIndex) {
                const auto& lhs = m_pendingStreamingRequests[lhsIndex];
                const auto& rhs = m_pendingStreamingRequests[rhsIndex];
                if (lhs.priority != rhs.priority) {
                    return lhs.priority > rhs.priority;
                }
                return lhs.request.groupGlobalIndex < rhs.request.groupGlobalIndex;
            };

            auto swapEntries = [this](uint32_t a, uint32_t b) {
                std::swap(m_pendingStreamingRequests[a], m_pendingStreamingRequests[b]);
                m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[a].request.groupGlobalIndex] = a;
                m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[b].request.groupGlobalIndex] = b;
            };

            auto siftUp = [&](uint32_t index) {
                while (index > 0u) {
                    const uint32_t parent = (index - 1u) >> 1u;
                    if (!higherPriority(index, parent)) {
                        break;
                    }
                    swapEntries(index, parent);
                    index = parent;
                }
                return index;
            };

            auto siftDown = [&](uint32_t index) {
                for (;;) {
                    const uint32_t left = index * 2u + 1u;
                    const uint32_t right = left + 1u;
                    uint32_t best = index;
                    if (left < m_pendingStreamingRequests.size() && higherPriority(left, best)) {
                        best = left;
                    }
                    if (right < m_pendingStreamingRequests.size() && higherPriority(right, best)) {
                        best = right;
                    }
                    if (best == index) {
                        break;
                    }
                    swapEntries(index, best);
                    index = best;
                }
            };

            m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = UINT32_MAX;
            if (heapIndex + 1u == m_pendingStreamingRequests.size()) {
                m_pendingStreamingRequests.pop_back();
            } else {
                m_pendingStreamingRequests[heapIndex] = m_pendingStreamingRequests.back();
                m_pendingStreamingRequests.pop_back();
                const uint32_t movedGroup = m_pendingStreamingRequests[heapIndex].request.groupGlobalIndex;
                m_pendingStreamingRequestHeapIndexByGroup[movedGroup] = heapIndex;
                const uint32_t adjustedIndex = siftUp(heapIndex);
                siftDown(adjustedIndex);
            }
        }
    }

    if (state == StreamingRequestState::PendingCpu && m_pendingStreamingRequestCount > 0u) {
        --m_pendingStreamingRequestCount;
    }
    if (state == StreamingRequestState::WaitingForPages && m_waitingForPagesRequestCount > 0u) {
        --m_waitingForPagesRequestCount;
    }
    if (m_streamingRequestsInProgressCount > 0u) {
        --m_streamingRequestsInProgressCount;
    }
    state = StreamingRequestState::None;
    {
        ZoneScopedN("CLodStreamingSystem::ClearStreamingRequestInProgress::EraseReadyCompletion");
        auto readyIt =
            m_readyStreamingCompletionsByGroup.find(groupIndex);
        if (readyIt != m_readyStreamingCompletionsByGroup.end()) {
            m_readyStreamingCompletionBytes -=
                std::min(
                    m_readyStreamingCompletionBytes,
                    CLodReadyCompletionStorageBytes(readyIt->second));
            m_readyStreamingCompletionsByGroup.erase(readyIt);
        }
    }
    if (groupIndex < m_readyStreamingCompletionRetryQueuedByGroup.size()) {
        m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 0u;
    }
    if (groupIndex <
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup.size()) {
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup[groupIndex] =
            0u;
    }
    if (groupIndex < m_readyStreamingCompletionWaitPageByGroup.size()) {
        m_readyStreamingCompletionWaitPageByGroup[groupIndex] = UINT32_MAX;
        m_readyStreamingCompletionWaitKeyByGroup[groupIndex] =
            kInvalidCLodMeshPageKey;
    }
    if (groupIndex < m_readyStreamingCompletionWaitParentByGroup.size()) {
        m_readyStreamingCompletionWaitParentByGroup[groupIndex] = UINT32_MAX;
    }
    if (groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()) {
        m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = UINT32_MAX;
    }
    if (groupIndex < m_pendingStreamingRequestGenerationByGroup.size()) {
        ++m_pendingStreamingRequestGenerationByGroup[groupIndex];
    }
    if (groupIndex < m_waitingForPagesRequestIndexByGroup.size()) {
        ZoneScopedN("CLodStreamingSystem::ClearStreamingRequestInProgress::RemoveWaitingForPagesEntry");
        const uint32_t waitingIndex = m_waitingForPagesRequestIndexByGroup[groupIndex];
        if (waitingIndex != UINT32_MAX &&
            waitingIndex < m_waitingForPagesRequests.size() &&
            m_waitingForPagesRequests[waitingIndex].request.groupGlobalIndex == groupIndex) {
            if (waitingIndex + 1u != m_waitingForPagesRequests.size()) {
                m_waitingForPagesRequests[waitingIndex] = m_waitingForPagesRequests.back();
                const uint32_t movedGroup = m_waitingForPagesRequests[waitingIndex].request.groupGlobalIndex;
                if (movedGroup < m_waitingForPagesRequestIndexByGroup.size()) {
                    m_waitingForPagesRequestIndexByGroup[movedGroup] = waitingIndex;
                }
            }
            m_waitingForPagesRequests.pop_back();
        }
        m_waitingForPagesRequestIndexByGroup[groupIndex] = UINT32_MAX;
    }
    RecordStreamingTerminal(groupIndex);
}

uint32_t CLodStreamingSystem::GetPendingLoadPriority(uint32_t groupIndex) const {
    return groupIndex < m_pendingLoadPriorityByGroup.size() ? m_pendingLoadPriorityByGroup[groupIndex] : 0u;
}

void CLodStreamingSystem::SetPendingLoadPriority(uint32_t groupIndex, uint32_t priority) {
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    m_pendingLoadPriorityByGroup[groupIndex] = priority;
}

void CLodStreamingSystem::ClearPendingLoadPriority(uint32_t groupIndex) {
    if (groupIndex < m_pendingLoadPriorityByGroup.size()) {
        m_pendingLoadPriorityByGroup[groupIndex] = 0u;
    }
}

void CLodStreamingSystem::PushOrUpdatePendingStreamingRequest(const CLodStreamingRequest& req, uint32_t priority) {
    const uint32_t groupIndex = req.groupGlobalIndex;
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }

    MarkStreamingRequestPending(groupIndex);
    SetPendingLoadPriority(groupIndex, priority);
    const uint32_t generation = ++m_pendingStreamingRequestGenerationByGroup[groupIndex];

    auto higherPriority = [this](uint32_t lhsIndex, uint32_t rhsIndex) {
        const auto& lhs = m_pendingStreamingRequests[lhsIndex];
        const auto& rhs = m_pendingStreamingRequests[rhsIndex];
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.request.groupGlobalIndex < rhs.request.groupGlobalIndex;
    };

    auto swapEntries = [this](uint32_t a, uint32_t b) {
        std::swap(m_pendingStreamingRequests[a], m_pendingStreamingRequests[b]);
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[a].request.groupGlobalIndex] = a;
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[b].request.groupGlobalIndex] = b;
    };

    auto siftUp = [&](uint32_t index) {
        while (index > 0u) {
            const uint32_t parent = (index - 1u) >> 1u;
            if (!higherPriority(index, parent)) {
                break;
            }
            swapEntries(index, parent);
            index = parent;
        }
    };

    auto siftDown = [&](uint32_t index) {
        for (;;) {
            const uint32_t left = index * 2u + 1u;
            const uint32_t right = left + 1u;
            uint32_t best = index;
            if (left < m_pendingStreamingRequests.size() && higherPriority(left, best)) {
                best = left;
            }
            if (right < m_pendingStreamingRequests.size() && higherPriority(right, best)) {
                best = right;
            }
            if (best == index) {
                break;
            }
            swapEntries(index, best);
            index = best;
        }
    };

    uint32_t heapIndex = m_pendingStreamingRequestHeapIndexByGroup[groupIndex];
    if (heapIndex == UINT32_MAX || heapIndex >= m_pendingStreamingRequests.size()
        || m_pendingStreamingRequests[heapIndex].request.groupGlobalIndex != groupIndex) {
        PendingStreamingRequest pending{};
        pending.request = req;
        pending.priority = priority;
        pending.generation = generation;
        pending.lastObservedTick = m_streamingDiagnosticTick;
        m_pendingStreamingRequests.push_back(pending);
        heapIndex = static_cast<uint32_t>(m_pendingStreamingRequests.size() - 1u);
        m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = heapIndex;
        siftUp(heapIndex);
        return;
    }

    const uint32_t oldPriority = m_pendingStreamingRequests[heapIndex].priority;
    m_pendingStreamingRequests[heapIndex].request = req;
    m_pendingStreamingRequests[heapIndex].priority = priority;
    m_pendingStreamingRequests[heapIndex].generation = generation;
    m_pendingStreamingRequests[heapIndex].lastObservedTick =
        m_streamingDiagnosticTick;
    if (priority > oldPriority) {
        siftUp(heapIndex);
    } else if (priority < oldPriority) {
        siftDown(heapIndex);
    }
}

void CLodStreamingSystem::ParkStreamingRequestWaitingForPages(const PendingStreamingRequest& pending) {
    const uint32_t groupIndex = pending.request.groupGlobalIndex;
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    if (groupIndex >= m_streamingRequestStateByGroup.size()) {
        return;
    }

    MarkStreamingRequestWaitingForPages(groupIndex);
    SetPendingLoadPriority(groupIndex, pending.priority);

    uint32_t waitingIndex = groupIndex < m_waitingForPagesRequestIndexByGroup.size()
        ? m_waitingForPagesRequestIndexByGroup[groupIndex]
        : UINT32_MAX;
    if (waitingIndex != UINT32_MAX &&
        waitingIndex < m_waitingForPagesRequests.size() &&
        m_waitingForPagesRequests[waitingIndex].request.groupGlobalIndex == groupIndex) {
        m_waitingForPagesRequests[waitingIndex] = pending;
        return;
    }

    PendingStreamingRequest parked = pending;
    parked.generation = groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
        ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
        : pending.generation;
    m_waitingForPagesRequests.push_back(parked);
    waitingIndex = static_cast<uint32_t>(m_waitingForPagesRequests.size() - 1u);
    m_waitingForPagesRequestIndexByGroup[groupIndex] = waitingIndex;
}

void CLodStreamingSystem::RequeueWaitingForPagesRequests(uint32_t maxRequests) {
    if (maxRequests == 0u || m_waitingForPagesRequests.empty()) {
        return;
    }

    uint32_t requeued = 0u;
    uint32_t scanCount = std::min<uint32_t>(maxRequests, static_cast<uint32_t>(m_waitingForPagesRequests.size()));
    while (scanCount-- > 0u && !m_waitingForPagesRequests.empty()) {
        PendingStreamingRequest pending = m_waitingForPagesRequests.back();
        m_waitingForPagesRequests.pop_back();

        const uint32_t groupIndex = pending.request.groupGlobalIndex;
        if (groupIndex < m_waitingForPagesRequestIndexByGroup.size()) {
            m_waitingForPagesRequestIndexByGroup[groupIndex] = UINT32_MAX;
        }
        if (groupIndex >= m_streamingRequestStateByGroup.size() ||
            groupIndex >= m_pendingStreamingRequestGenerationByGroup.size()) {
            continue;
        }
        if (m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::WaitingForPages) {
            continue;
        }
        if (pending.generation != m_pendingStreamingRequestGenerationByGroup[groupIndex]) {
            ClearStreamingRequestInProgress(groupIndex);
            ClearPendingLoadPriority(groupIndex);
            continue;
        }
        if (!IsGroupActive(groupIndex) || IsGroupResident(groupIndex)) {
            ClearStreamingRequestInProgress(groupIndex);
            ClearPendingLoadPriority(groupIndex);
            continue;
        }

        const uint32_t priority = std::max<uint32_t>(pending.priority, GetPendingLoadPriority(groupIndex));
        PushOrUpdatePendingStreamingRequest(pending.request, priority);
        ++requeued;
    }

    if (requeued != 0u) {
        TracyPlot("CLodStreaming.Service.RequeuedWaitingForPages", static_cast<int64_t>(requeued));
    }
}

void CLodStreamingSystem::RequeuePendingStreamingRequest(const PendingStreamingRequest& pending) {
    const uint32_t groupIndex = pending.request.groupGlobalIndex;
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    if (groupIndex >= m_streamingRequestStateByGroup.size() ||
        m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::PendingCpu) {
        return;
    }
    if (groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()) {
        const uint32_t heapIndex = m_pendingStreamingRequestHeapIndexByGroup[groupIndex];
        if (heapIndex != UINT32_MAX &&
            heapIndex < m_pendingStreamingRequests.size() &&
            m_pendingStreamingRequests[heapIndex].request.groupGlobalIndex == groupIndex) {
            return;
        }
    }

    auto higherPriority = [this](uint32_t lhsIndex, uint32_t rhsIndex) {
        const auto& lhs = m_pendingStreamingRequests[lhsIndex];
        const auto& rhs = m_pendingStreamingRequests[rhsIndex];
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.request.groupGlobalIndex < rhs.request.groupGlobalIndex;
    };

    auto swapEntries = [this](uint32_t a, uint32_t b) {
        std::swap(m_pendingStreamingRequests[a], m_pendingStreamingRequests[b]);
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[a].request.groupGlobalIndex] = a;
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[b].request.groupGlobalIndex] = b;
    };

    m_pendingStreamingRequests.push_back(pending);
    uint32_t index = static_cast<uint32_t>(m_pendingStreamingRequests.size() - 1u);
    m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = index;
    while (index > 0u) {
        const uint32_t parent = (index - 1u) >> 1u;
        if (!higherPriority(index, parent)) {
            break;
        }
        swapEntries(index, parent);
        index = parent;
    }
}

bool CLodStreamingSystem::RemovePendingStreamingRequestAt(
    uint32_t removeIndex,
    PendingStreamingRequest& outRequest) {
    if (removeIndex >= m_pendingStreamingRequests.size()) {
        return false;
    }
    auto higherPriority = [this](uint32_t lhsIndex, uint32_t rhsIndex) {
        const auto& lhs = m_pendingStreamingRequests[lhsIndex];
        const auto& rhs = m_pendingStreamingRequests[rhsIndex];
        if (lhs.priority != rhs.priority) {
            return lhs.priority > rhs.priority;
        }
        return lhs.request.groupGlobalIndex < rhs.request.groupGlobalIndex;
    };

    auto swapEntries = [this](uint32_t a, uint32_t b) {
        std::swap(m_pendingStreamingRequests[a], m_pendingStreamingRequests[b]);
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[a].request.groupGlobalIndex] = a;
        m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests[b].request.groupGlobalIndex] = b;
    };

    outRequest = m_pendingStreamingRequests[removeIndex];
    const uint32_t groupIndex = outRequest.request.groupGlobalIndex;
    if (groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()
        && m_pendingStreamingRequestHeapIndexByGroup[groupIndex] == removeIndex) {
        m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = UINT32_MAX;
    }

    if (m_pendingStreamingRequests.size() == 1u) {
        m_pendingStreamingRequests.pop_back();
        return true;
    }

    if (removeIndex == m_pendingStreamingRequests.size() - 1u) {
        m_pendingStreamingRequests.pop_back();
        return true;
    }

    m_pendingStreamingRequests[removeIndex] =
        m_pendingStreamingRequests.back();
    m_pendingStreamingRequests.pop_back();
    m_pendingStreamingRequestHeapIndexByGroup[
        m_pendingStreamingRequests[removeIndex].request.groupGlobalIndex] =
        removeIndex;

    uint32_t index = removeIndex;
    while (index > 0u) {
        const uint32_t parent = (index - 1u) >> 1u;
        if (!higherPriority(index, parent)) {
            break;
        }
        swapEntries(index, parent);
        index = parent;
    }
    if (index != removeIndex) {
        return true;
    }

    for (;;) {
        const uint32_t left = index * 2u + 1u;
        const uint32_t right = left + 1u;
        uint32_t best = index;
        if (left < m_pendingStreamingRequests.size() && higherPriority(left, best)) {
            best = left;
        }
        if (right < m_pendingStreamingRequests.size() && higherPriority(right, best)) {
            best = right;
        }
        if (best == index) {
            break;
        }
        swapEntries(index, best);
        index = best;
    }

    return true;
}

bool CLodStreamingSystem::PopHighestPriorityPendingStreamingRequest(
    PendingStreamingRequest& outRequest) {
    if (m_pendingStreamingRequests.empty()) {
        return false;
    }
    const uint32_t groupIndex =
        m_pendingStreamingRequests.front().request.groupGlobalIndex;
    if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
        auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
        diag.liveAtAdmission =
            diag.lastRequestTick != 0u &&
            m_streamingDiagnosticTick <=
                diag.lastRequestTick +
                    static_cast<uint64_t>(m_streamingReadbackRingSize + 2u);
    }
    return RemovePendingStreamingRequestAt(0u, outRequest);
}
