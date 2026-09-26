#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"
#include "VirtualGeometry/Streaming/CLodStreamingTraceInternals.h"

#include <algorithm>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <BasicTelemetry/Telemetry.h>
#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

void CLodStreamingSystem::CommitPendingResidencyPromotions(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions");

    if (m_pendingResidencyCommitGroups.empty()) {
        return;
    }

    std::vector<uint32_t> groups;
    {
        ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::CollectGroups");
        groups.reserve(m_pendingResidencyCommitGroups.size());
        for (uint32_t groupIndex : m_pendingResidencyCommitGroups) {
            groups.push_back(groupIndex);
        }
        m_pendingResidencyCommitGroups.clear();
    }
    std::unordered_map<uint32_t, uint32_t> groupDepths;
    groupDepths.reserve(groups.size());
    for (uint32_t group : groups) {
        groupDepths.emplace(
            group, SelectedAncestorDepth(group, meshManager));
    }
    std::stable_sort(
        groups.begin(),
        groups.end(),
        [&groupDepths](uint32_t lhs, uint32_t rhs) {
            return groupDepths.at(lhs) < groupDepths.at(rhs);
        });

    TracyPlot(
        "CLodStreaming.ApplyPromotions.InputGroups",
        static_cast<int64_t>(groups.size()));

    {
        ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions");
        const uint64_t completedUploadFence = [&]() {
            ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::ReadCompletedUploadFence");
            return m_streamingUploadCompletionFenceHandle.IsValid()
                ? m_streamingUploadCompletionFenceHandle.GetCompletedValue()
                : UINT64_MAX;
        }();

        uint32_t inactiveGroups = 0u;
        uint32_t missingOwnedPages = 0u;
        uint32_t uploadFenceDeferrals = 0u;
        uint32_t pagePromotionDeferrals = 0u;
        uint32_t parentResidencyDeferrals = 0u;
        uint32_t promotedGroups = 0u;
        uint64_t promotedPageSlots = 0u;
        uint32_t shadowPromotionGroups = 0u;
        std::optional<std::unordered_set<uint32_t>> groupsAwaitingFenceSeal;
        std::optional<std::unordered_set<uint32_t>> groupsWithRetainedUploadBatch;
        std::unordered_set<uint32_t> promotedThisBatch;
        promotedThisBatch.reserve(groups.size());

        for (uint32_t groupIndex : groups) {
            {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::ValidateGroup");
                if (groupIndex >= m_streamingStorageGroupCapacity || !IsGroupActive(groupIndex)) {
                    ++inactiveGroups;
                    m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                    ClearStreamingRequestInProgress(groupIndex);
                    ClearPendingLoadPriority(groupIndex);
                    continue;
                }
            }

            const auto ownedPagesIt = m_groupOwnedPages.find(groupIndex);
            if (ownedPagesIt == m_groupOwnedPages.end()) {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::DiscardMissingOwnedPages");
                ++missingOwnedPages;
                m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
                continue;
            }

            if (!IsGroupSelectedParentResident(groupIndex, meshManager)) {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::DeferForParents");
                m_pendingResidencyCommitGroups.insert(groupIndex);
                if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
                    ++m_streamingDiagnosticsByGroup[groupIndex].promotionDeferrals;
                }
                ++m_streamingDiagnosticsPromotionDeferralsThisFrame;
                ++parentResidencyDeferrals;
                continue;
            }

            bool hasUploadFence = false;
            bool uploadBatchReady = false;
            {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::ResolveGroupFence");
                const auto resolvedUploadFenceIt =
                    m_pendingResidencyUploadFenceByGroup.find(groupIndex);
                hasUploadFence =
                    resolvedUploadFenceIt != m_pendingResidencyUploadFenceByGroup.end();
                uploadBatchReady = hasUploadFence &&
                    completedUploadFence >= resolvedUploadFenceIt->second;
            }

            bool pagesReady = false;
            if (uploadBatchReady) {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::PromotePhysicalPages");
                promotedPageSlots += ownedPagesIt->second.size();
                pagesReady = PromoteGroupPagesAfterUploadDrain(groupIndex);
            }

            if (!uploadBatchReady || !pagesReady) {
                if (!hasUploadFence) {
                    ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::DiagnoseMissingFence");
                    if (!groupsAwaitingFenceSeal) {
                        ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::BuildMissingFenceLookup");
                        groupsAwaitingFenceSeal.emplace(
                            m_residencyGroupsAwaitingUploadFence.begin(),
                            m_residencyGroupsAwaitingUploadFence.end());
                        groupsWithRetainedUploadBatch.emplace();
                        size_t affectedGroupCount = 0u;
                        for (const auto& batch : m_outstandingUploadBatches) {
                            if (batch && batch->ticket) {
                                affectedGroupCount += batch->affectedGroups.size();
                            }
                        }
                        groupsWithRetainedUploadBatch->reserve(affectedGroupCount);
                        for (const auto& batch : m_outstandingUploadBatches) {
                            if (!batch || !batch->ticket) {
                                continue;
                            }
                            groupsWithRetainedUploadBatch->insert(
                                batch->affectedGroups.begin(),
                                batch->affectedGroups.end());
                        }
                    }
                    const bool awaitingSeal = groupsAwaitingFenceSeal->contains(groupIndex);
                    const bool hasBatchTicket =
                        groupsWithRetainedUploadBatch->contains(groupIndex);
                    if (!awaitingSeal && !hasBatchTicket) {
                        spdlog::critical(
                            "CLod streaming invariant: pending-commit group {} has no published or retained upload batch",
                            groupIndex);
#if BUILD_TYPE == BUILD_TYPE_DEBUG
                        __debugbreak();
#endif
                    }
                }

                {
                    ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::RequeueDeferred");
                    m_pendingResidencyCommitGroups.insert(groupIndex);
                    if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
                        ++m_streamingDiagnosticsByGroup[groupIndex].promotionDeferrals;
                    }
                    ++m_streamingDiagnosticsPromotionDeferralsThisFrame;
                }
                uploadFenceDeferrals += !uploadBatchReady ? 1u : 0u;
                pagePromotionDeferrals += uploadBatchReady && !pagesReady ? 1u : 0u;
                continue;
            }

            uint32_t selectedParent = 0u;
            const bool parentPromotedInThisBatch =
                meshManager != nullptr &&
                meshManager->TryGetCLodParentGroup(
                    groupIndex, selectedParent) &&
                promotedThisBatch.contains(selectedParent);
            bool residencyChanged = false;
            {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::SetResident");
                residencyChanged = SetGroupResidentBit(groupIndex, true);
            }
            if (residencyChanged) {
                promotedThisBatch.insert(groupIndex);
                m_transactionalChildPromotions +=
                    parentPromotedInThisBatch ? 1u : 0u;
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::QueueVirtualShadowUpgrade");
                QueueVirtualShadowUpgradeForPromotion(groupIndex);
                WakeReadyCompletionsForParent(groupIndex);
                ++shadowPromotionGroups;
            }

            {
                ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions::FinalizeBookkeeping");
                m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                RecordStreamingPromoted(groupIndex);
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
            }
            ++promotedGroups;
        }

        TracyPlot("CLodStreaming.ApplyPromotions.InactiveGroups", static_cast<int64_t>(inactiveGroups));
        TracyPlot("CLodStreaming.ApplyPromotions.MissingOwnedPages", static_cast<int64_t>(missingOwnedPages));
        TracyPlot("CLodStreaming.ApplyPromotions.UploadFenceDeferrals", static_cast<int64_t>(uploadFenceDeferrals));
        TracyPlot("CLodStreaming.ApplyPromotions.PagePromotionDeferrals", static_cast<int64_t>(pagePromotionDeferrals));
        TracyPlot("CLodStreaming.ApplyPromotions.ParentResidencyDeferrals", static_cast<int64_t>(parentResidencyDeferrals));
        TracyPlot("CLodStreaming.ApplyPromotions.PromotedGroups", static_cast<int64_t>(promotedGroups));
        TracyPlot("CLodStreaming.ApplyPromotions.PageSlotsVisited", static_cast<int64_t>(promotedPageSlots));
        TracyPlot("CLodStreaming.ApplyPromotions.ShadowPromotionGroups", static_cast<int64_t>(shadowPromotionGroups));
    }
}

void CLodStreamingSystem::ReconcileStaleDiskIoRequests(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::ReconcileStaleDiskIoRequests");

    if (meshManager == nullptr || m_streamingRequestsInProgressCount == 0u) {
        return;
    }

    const auto debugStats = meshManager->GetCLodStreamingDebugStats();
    if (debugStats.queuedRequests != 0u ||
        debugStats.queuedOrInFlightGroups != 0u ||
        debugStats.completedResults != 0u) {
        return;
    }

    uint32_t cleared = 0u;
    uint32_t releasedPreallocations = 0u;
    for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(m_streamingRequestStateByGroup.size()); ++groupIndex) {
        if (m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::DiskIo) {
            continue;
        }
        if (m_pendingResidencyCommitGroups.find(groupIndex) != m_pendingResidencyCommitGroups.end()) {
            continue;
        }
        if (m_readyStreamingCompletionsByGroup.find(groupIndex) != m_readyStreamingCompletionsByGroup.end()) {
            continue;
        }

        if (auto preAllocIt = m_preAllocatedPagesByGroup.find(groupIndex);
            preAllocIt != m_preAllocatedPagesByGroup.end()) {
            ReleasePreAllocatedPages(preAllocIt->second, meshManager);
            m_preAllocatedPagesByGroup.erase(preAllocIt);
            ++releasedPreallocations;
        }

        ClearStreamingRequestInProgress(groupIndex);
        ClearPendingLoadPriority(groupIndex);
        ++cleared;
    }

    if (cleared == 0u) {
        return;
    }

    static uint64_t s_lastStaleDiskIoLogTick = 0u;
    if (m_streamingDiagnosticTick >= s_lastStaleDiskIoLogTick + 120u) {
        s_lastStaleDiskIoLogTick = m_streamingDiagnosticTick;
        spdlog::warn(
            "CLod streaming diag[tick={}]: cleared {} stale DiskIo request states after MeshManager reported no queued/in-flight/completed work; releasedPreallocations={} cpuPending={} cpuInProgress={}",
            m_streamingDiagnosticTick,
            cleared,
            releasedPreallocations,
            m_pendingStreamingRequestCount,
            m_streamingRequestsInProgressCount);
    }
}

bool CLodStreamingSystem::PromoteGroupPagesAfterUploadDrain(uint32_t groupIndex) {
    ZoneScopedN("CLodStreamingSystem::PromoteGroupPagesAfterUploadDrain");

    const auto pagesIt = m_groupOwnedPages.find(groupIndex);
    if (pagesIt == m_groupOwnedPages.end()) {
        return true;
    }

    const auto keysIt = m_groupOwnedMeshPageKeys.find(groupIndex);
    bool waitingForSharedPendingPage = false;
    for (uint32_t seg = 0; seg < static_cast<uint32_t>(pagesIt->second.size()); ++seg) {
        const uint32_t page = pagesIt->second[seg];
        if (page == ~0u || page >= m_pageState.size()) {
            continue;
        }

        const uint64_t key = keysIt != m_groupOwnedMeshPageKeys.end() && seg < static_cast<uint32_t>(keysIt->second.size())
            ? keysIt->second[seg]
            : kInvalidCLodMeshPageKey;

        if (IsPhysicalPageResidentForKey(page, key)) {
            ZoneScopedN("CLodStreamingSystem::PromoteGroupPagesAfterUploadDrain::AttachSharedResidentPage");
            bool insertedGroup = false;
            if (page < m_pageResidentGroups.size()) {
                insertedGroup = m_pageResidentGroups[page].insert(groupIndex).second;
            }
            if (insertedGroup && key != kInvalidCLodMeshPageKey) {
                m_residentMeshPageRefCounts[key]++;
            }
            ReleasePendingMeshPageReference(page, key);
            if (!IsPhysicalPagePinnedStorage(page)) {
                PageLruForPage(page).Insert(page);
            }
            continue;
        }

        if (m_pageState[page] != CLodPhysicalPageState::PreAllocatedCpuUpload &&
            m_pageState[page] != CLodPhysicalPageState::PendingDirectStorageWrite) {
            continue;
        }

        if (page < m_pendingPageOwnerGroup.size() &&
            m_pendingPageOwnerGroup[page] != ~0u &&
            m_pendingPageOwnerGroup[page] != groupIndex) {
            ZoneScopedN("CLodStreamingSystem::PromoteGroupPagesAfterUploadDrain::WaitForSharedPendingPage");
            if (IsPhysicalPagePendingForKey(page, key)) {
                waitingForSharedPendingPage = true;
                spdlog::debug(
                    "CLod streaming: group {} waiting to promote shared pending page {} key {} owned by group {}",
                    groupIndex,
                    page,
                    key,
                    m_pendingPageOwnerGroup[page]);
            }
            continue;
        }

        ZoneScopedN("CLodStreamingSystem::PromoteGroupPagesAfterUploadDrain::CommitPhysicalPage");
        m_pageState[page] = CLodPhysicalPageState::Resident;
        if (page < m_pendingPageOwnerGroup.size()) {
            m_pendingPageOwnerGroup[page] = ~0u;
            m_pendingPageOwnerSegment[page] = 0u;
        }
        if (key != kInvalidCLodMeshPageKey) {
            m_residentMeshPageToPhysicalPage[key] = page;
            m_residentMeshPageRefCounts[key]++;
            ReleasePendingMeshPageReference(page, key);
        }
        if (page < m_pageResidentGroups.size()) {
            m_pageResidentGroups[page].insert(groupIndex);
        }
        if (!IsPhysicalPagePinnedStorage(page)) {
            PageLruForPage(page).Insert(page);
        }
        WakeReadyCompletionsForPage(page, key);
    }

    return !waitingForSharedPendingPage;
}

void CLodStreamingSystem::ForceGroupNonResident(uint32_t groupIndex, ICLodGeometryStorage* meshManager, bool clearPageMapEntries) {
    SetGroupResidentBit(groupIndex, false);
    ReleaseGroupResidency(groupIndex, meshManager, clearPageMapEntries);
    m_pendingResidencyCommitGroups.erase(groupIndex);
}

void CLodStreamingSystem::ForceGroupAndDescendantsNonResident(
    uint32_t groupIndex,
    ICLodGeometryStorage* meshManager,
    bool clearPageMapEntries) {
    if (meshManager == nullptr) {
        ForceGroupNonResident(groupIndex, meshManager, clearPageMapEntries);
        return;
    }

    // Build a child-first order so shared pages remain attributed to a valid
    // resident owner until every finer dependency has been removed.
    std::vector<std::pair<uint32_t, bool>> traversal;
    std::vector<uint32_t> evictionOrder;
    std::unordered_set<uint32_t> visited;
    traversal.emplace_back(groupIndex, false);
    while (!traversal.empty()) {
        const auto [current, expanded] = traversal.back();
        traversal.pop_back();
        if (expanded) {
            evictionOrder.push_back(current);
            continue;
        }
        if (!visited.insert(current).second) {
            continue;
        }

        traversal.emplace_back(current, true);
        std::vector<uint32_t> children;
        meshManager->GetCLodChildGroups(current, children);
        for (uint32_t child : children) {
            if (child != current) {
                traversal.emplace_back(child, false);
            }
        }
    }

    for (uint32_t current : evictionOrder) {
        // Leave already-non-resident descendants' in-flight uploads intact;
        // the promotion gate below will hold them until their parents return.
        // The requested root is always released because it owns the physical
        // page that initiated this eviction.
        if (current == groupIndex || IsGroupResident(current)) {
            ForceGroupNonResident(current, meshManager, clearPageMapEntries);
        }
    }
}

bool CLodStreamingSystem::IsGroupSelectedParentResident(uint32_t groupIndex, ICLodGeometryStorage* meshManager) const {
    if (meshManager == nullptr) {
        return true;
    }

    uint32_t parent = 0u;
    if (!meshManager->TryGetCLodParentGroup(groupIndex, parent) ||
        IsGroupResident(parent)) {
        return true;
    }

    // Structural groups have no payload and are always usable by the shader
    // regardless of the streamed-residency bit.
    const br::render::CLodGroupStreamingInfo info =
        meshManager->GetCLodGroupStreamingInfo(parent);
    return info.valid && info.pageCount == 0u;
}

bool CLodStreamingSystem::IsGroupSelectedParentResidentOrCommitReady(
    uint32_t groupIndex,
    ICLodGeometryStorage* meshManager) const {
    if (IsGroupSelectedParentResident(groupIndex, meshManager) ||
        meshManager == nullptr) {
        return true;
    }

    uint32_t parent = 0u;
    if (!meshManager->TryGetCLodParentGroup(groupIndex, parent)) {
        return true;
    }

    // Commit-ready means the parent's pages and render metadata have already
    // been validated and staged. Its upload fence may be sealed later in this
    // service transaction, but no further page allocation can invalidate it.
    return m_pendingResidencyCommitGroups.contains(parent) &&
        m_groupOwnedPages.contains(parent) &&
        m_groupCommittedPageMaps.contains(parent);
}

uint32_t CLodStreamingSystem::SelectedAncestorDepth(
    uint32_t groupIndex,
    ICLodGeometryStorage* meshManager) const {
    if (meshManager == nullptr) {
        return 0u;
    }

    uint32_t depth = 0u;
    uint32_t current = groupIndex;
    const uint32_t maxHops =
        std::max<uint32_t>(m_streamingStorageGroupCapacity, 1u);
    for (uint32_t hop = 0u; hop < maxHops; ++hop) {
        uint32_t parent = 0u;
        if (!meshManager->TryGetCLodParentGroup(current, parent) ||
            parent == current) {
            break;
        }
        ++depth;
        current = parent;
    }
    return depth;
}

void CLodStreamingSystem::TouchGroupPages(uint32_t groupIndex) {
    ZoneScopedN("CLodStreamingSystem::TouchGroupPages");

    auto it = m_groupOwnedPages.find(groupIndex);
    if (it != m_groupOwnedPages.end()) {
        for (uint32_t page : it->second) {
            if (page != ~0u) {
                PageLruForPage(page).Touch(page);
            }
        }
    }

    uint32_t current = groupIndex;
    for (size_t hop = 0; hop < m_streamingStorageGroupCapacity; ++hop) {
        uint32_t parent = 0;
        if (!TryGetCachedParentGroup(current, parent) || parent == current) {
            break;
        }

        auto pagesIt = m_groupOwnedPages.find(parent);
        if (pagesIt != m_groupOwnedPages.end()) {
            for (uint32_t page : pagesIt->second) {
                if (page != ~0u) {
                    PageLruForPage(page).Touch(page);
                }
            }
        }
        current = parent;
    }
}

