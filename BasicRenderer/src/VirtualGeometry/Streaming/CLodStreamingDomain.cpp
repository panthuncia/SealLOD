#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"

#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

void CLodStreamingSystem::RebuildStreamingDomainFromSnapshot(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::RebuildStreamingDomainFromSnapshot");
    if (meshManager == nullptr) {
        return;
    }

    br::render::CLodStreamingDomainSnapshot snapshot{};
    {
        ZoneScopedN("CLodStreamingSystem::RebuildStreamingDomainFromSnapshot::GetDomainSnapshot");
        meshManager->GetCLodStreamingDomainSnapshot(snapshot);
    }

    std::fill(m_streamingActiveGroupsBitsCpu.begin(), m_streamingActiveGroupsBitsCpu.end(), 0u);
    std::fill(m_streamingPinnedGroupsBitsCpu.begin(), m_streamingPinnedGroupsBitsCpu.end(), 0u);
    std::fill(m_streamingResidencyInitializedBitsCpu.begin(), m_streamingResidencyInitializedBitsCpu.end(), 0u);
    std::fill(m_streamingNonResidentBitsCpu.begin(), m_streamingNonResidentBitsCpu.end(), 0u);
    std::fill(m_parentGroupByGroup.begin(), m_parentGroupByGroup.end(), UINT32_MAX);
    for (uint32_t word : m_usedGroupsWordsCpu) {
        if (word < m_usedGroupsBitsCpu.size()) {
            m_usedGroupsBitsCpu[word] = 0u;
        }
    }
    m_usedGroupsWordsCpu.clear();
    m_streamingResidentGroupsCount = 0u;

    EnsureStreamingStorageCapacity(snapshot.maxGroupIndex);
    m_streamingActiveGroupScanCount = snapshot.maxGroupIndex;

    for (const auto& range : snapshot.activeRanges) {
        const uint32_t rangeBegin = std::min(range.groupsBase, m_streamingStorageGroupCapacity);
        const uint32_t rangeEnd = std::min(range.groupsBase + range.groupCount, m_streamingStorageGroupCapacity);
        for (uint32_t groupIndex = rangeBegin; groupIndex < rangeEnd; ++groupIndex) {
            m_streamingActiveGroupsBitsCpu[BitWordAddress(groupIndex)] |= BitMask(groupIndex);
            m_streamingNonResidentBitsCpu[BitWordAddress(groupIndex)] |= BitMask(groupIndex);
        }
    }
    for (const auto& range : snapshot.coarsestRanges) {
        if ((range.branches & m_activeBranches) == 0u) {
            continue;
        }
        const uint32_t rangeBegin = std::min(range.groupsBase, m_streamingStorageGroupCapacity);
        const uint32_t rangeEnd = std::min(range.groupsBase + range.groupCount, m_streamingStorageGroupCapacity);
        for (uint32_t groupIndex = rangeBegin; groupIndex < rangeEnd; ++groupIndex) {
            m_streamingPinnedGroupsBitsCpu[BitWordAddress(groupIndex)] |= BitMask(groupIndex);
        }
    }

    // Snapshot reconstruction is the new owner's equivalent of replaying all
    // historical ActiveRangeAdded events. In particular, pinned/coarsest roots
    // must become resident (or be queued) before traversal can refine safely.
    uint32_t initializedGroups = 0u;
    uint32_t queuedPinnedGroups = 0u;
    for (const auto& range : snapshot.activeRanges) {
        InitializeActiveRange(
            meshManager,
            range.groupsBase,
            range.groupCount,
            initializedGroups,
            queuedPinnedGroups);
    }
    MarkStreamingActiveGroupsBitsDirty();
    MarkStreamingNonResidentBitsDirtyAll();
    TracyPlot("CLodStreaming.Domain.FallbackFullReset", static_cast<int64_t>(1));
    TracyPlot("CLodStreaming.Domain.SnapshotInitializedGroups", static_cast<int64_t>(initializedGroups));
    TracyPlot("CLodStreaming.Domain.SnapshotQueuedPinnedGroups", static_cast<int64_t>(queuedPinnedGroups));
}

void CLodStreamingSystem::ProcessStreamingDomainEvents() {
    ZoneScopedN("CLodStreamingSystem::ProcessStreamingDomainEvents");
    ICLodGeometryStorage* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingDomainEvents::GetMeshManager");
        meshManager = m_geometryStorage;
    }

    if (meshManager == nullptr) {
        return;
    }

    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingDomainEvents::InitializePageLru");
        InitializePageLru(meshManager);
    }

    const uint32_t requestedBranches = m_requestedActiveBranches.load(std::memory_order_acquire);
    if (m_streamingDomainFullResetPending) {
        m_streamingDomainFullResetPending = false;
        m_activeBranches = requestedBranches;
        meshManager->SetCLodActiveBranches(m_activeBranches);
        RebuildStreamingDomainFromSnapshot(meshManager);
    } else if (requestedBranches != m_activeBranches) {
        ApplyActiveBranches(meshManager, requestedBranches);
    }

    uint64_t eventGeneration = 0;
    meshManager->DrainCLodStreamingDomainEvents(m_streamingDomainEventScratch, eventGeneration);
    if (m_streamingDomainEventScratch.empty()) {
        return;
    }
    m_lastStreamingDomainEventGeneration = eventGeneration;
    TracyPlot("CLodStreaming.Domain.EventsDrained", static_cast<int64_t>(m_streamingDomainEventScratch.size()));
    TracyPlot("CLodStreaming.Domain.EventGeneration", static_cast<int64_t>(eventGeneration));

    auto setBitRange = [this](std::vector<uint32_t>& bits, uint32_t begin, uint32_t count, bool enabled) {
        const uint32_t end = std::min(begin + count, m_streamingStorageGroupCapacity);
        for (uint32_t groupIndex = begin; groupIndex < end; ++groupIndex) {
            const uint32_t word = BitWordAddress(groupIndex);
            const uint32_t mask = BitMask(groupIndex);
            if (word >= bits.size()) {
                continue;
            }
            if (enabled) {
                bits[word] |= mask;
            } else {
                bits[word] &= ~mask;
            }
        }
    };

    auto releaseRemovedRange = [this, meshManager](uint32_t begin, uint32_t count) {
        const uint32_t end = std::min(begin + count, m_streamingStorageGroupCapacity);
        for (uint32_t groupIndex = begin; groupIndex < end; ++groupIndex) {
            ReleasePinnedStorageForGroup(groupIndex, meshManager);
            ClearStreamingRequestInProgress(groupIndex);
            ClearPendingLoadPriority(groupIndex);
        }
    };

    uint32_t activeRangesAdded = 0;
    uint32_t activeRangesRemoved = 0;
    uint32_t initializedGroups = 0;
    uint32_t queuedPinnedGroups = 0;

    for (const auto& event : m_streamingDomainEventScratch) {
        if (event.kind == br::render::CLodStreamingDomainEventKind::FullReset) {
            spdlog::warn("CLod streaming: processing full domain reset fallback event");
            ClearVirtualShadowUpgradeState();
            RebuildStreamingDomainFromSnapshot(meshManager);
            continue;
        }

        const uint32_t rangeEnd = event.groupsBase + event.groupCount;
        EnsureStreamingStorageCapacity(rangeEnd);
        m_streamingActiveGroupScanCount = std::max(m_streamingActiveGroupScanCount, rangeEnd);

        switch (event.kind) {
        case br::render::CLodStreamingDomainEventKind::SharedMeshAdded:
            break;
        case br::render::CLodStreamingDomainEventKind::ActiveRangeAdded:
            ++activeRangesAdded;
            setBitRange(m_streamingActiveGroupsBitsCpu, event.groupsBase, event.groupCount, true);
            for (const auto& pinnedRange : event.coarsestRanges) {
                if ((pinnedRange.branches & m_activeBranches) != 0u) {
                    setBitRange(m_streamingPinnedGroupsBitsCpu, pinnedRange.groupsBase, pinnedRange.groupCount, true);
                }
            }
            MarkStreamingActiveGroupsBitsDirty();
            InitializeActiveRange(
                meshManager,
                event.groupsBase,
                event.groupCount,
                initializedGroups,
                queuedPinnedGroups);
            break;
        case br::render::CLodStreamingDomainEventKind::ActiveRangeRemoved:
            ++activeRangesRemoved;
            setBitRange(m_streamingActiveGroupsBitsCpu, event.groupsBase, event.groupCount, false);
            for (const auto& pinnedRange : event.coarsestRanges) {
                setBitRange(m_streamingPinnedGroupsBitsCpu, pinnedRange.groupsBase, pinnedRange.groupCount, false);
            }
            MarkStreamingActiveGroupsBitsDirty();
            releaseRemovedRange(event.groupsBase, event.groupCount);
            break;
        default:
            break;
        }
    }

    TracyPlot("CLodStreaming.Domain.ActiveRangesAdded", static_cast<int64_t>(activeRangesAdded));
    TracyPlot("CLodStreaming.Domain.ActiveRangesRemoved", static_cast<int64_t>(activeRangesRemoved));
    TracyPlot("CLodStreaming.Domain.InitializedActiveGroups", static_cast<int64_t>(initializedGroups));
    TracyPlot("CLodStreaming.Domain.QueuedPinnedGroups", static_cast<int64_t>(queuedPinnedGroups));
}

void CLodStreamingSystem::ReleasePinnedStorageForGroup(uint32_t groupIndex, ICLodGeometryStorage* meshManager) {
    const uint32_t word = BitWordAddress(groupIndex);
    const uint32_t mask = BitMask(groupIndex);
    if (UsesPinnedStorage(groupIndex)) {
        if (IsGroupResident(groupIndex)) {
            meshManager->EvictCLodGroupResidency(groupIndex, true);
        }
        ReleaseOwnedPagesForGroup(groupIndex, meshManager);
    }

    if (auto preAllocIt = m_preAllocatedPagesByGroup.find(groupIndex);
        preAllocIt != m_preAllocatedPagesByGroup.end() && preAllocIt->second.usesPinnedStorage && !IsStreamingRequestInProgress(groupIndex)) {
        ReleasePreAllocatedPages(preAllocIt->second, meshManager);
        m_preAllocatedPagesByGroup.erase(preAllocIt);
    }

    if (word < m_streamingResidencyInitializedBitsCpu.size()) {
        m_streamingResidencyInitializedBitsCpu[word] &= ~mask;
    }
}

// A view mode change (a graph rebuild) can change which branches of forked DAGs are drawn. Parents follow the new
// branches; the branches no longer drawn give up their pinned coarsest groups, and the ones newly drawn pin theirs.
void CLodStreamingSystem::ApplyActiveBranches(ICLodGeometryStorage* meshManager, uint32_t branches) {
    ZoneScopedN("CLodStreamingSystem::ApplyActiveBranches");
    m_activeBranches = branches;
    meshManager->SetCLodActiveBranches(branches);
    std::fill(m_parentGroupByGroup.begin(), m_parentGroupByGroup.end(), UINT32_MAX);

    br::render::CLodStreamingDomainSnapshot snapshot{};
    meshManager->GetCLodStreamingDomainSnapshot(snapshot);
    uint32_t pinned = 0u;
    uint32_t unpinned = 0u;
    std::vector<uint32_t> reinitialize;
    for (const auto& range : snapshot.coarsestRanges) {
        const bool wantPinned = (range.branches & branches) != 0u;
        const uint32_t end = std::min(range.groupsBase + range.groupCount, m_streamingStorageGroupCapacity);
        for (uint32_t groupIndex = range.groupsBase; groupIndex < end; ++groupIndex) {
            const uint32_t word = BitWordAddress(groupIndex);
            const uint32_t mask = BitMask(groupIndex);
            if (word >= m_streamingPinnedGroupsBitsCpu.size() || IsGroupPinned(groupIndex) == wantPinned) {
                continue;
            }
            if (wantPinned) {
                m_streamingPinnedGroupsBitsCpu[word] |= mask;
                ++pinned;
                if (!IsGroupResident(groupIndex) && !IsStreamingRequestInProgress(groupIndex)) {
                    m_streamingResidencyInitializedBitsCpu[word] &= ~mask;
                    reinitialize.push_back(groupIndex);
                }
            } else {
                m_streamingPinnedGroupsBitsCpu[word] &= ~mask;
                ++unpinned;
                if (UsesPinnedStorage(groupIndex) && !IsStreamingRequestInProgress(groupIndex)) {
                    ReleasePinnedStorageForGroup(groupIndex, meshManager);
                    reinitialize.push_back(groupIndex);
                }
            }
        }
    }
    uint32_t initializedGroups = 0u;
    uint32_t queuedPinnedGroups = 0u;
    for (const uint32_t groupIndex : reinitialize) {
        InitializeActiveRange(meshManager, groupIndex, 1u, initializedGroups, queuedPinnedGroups);
    }
    MarkStreamingActiveGroupsBitsDirty();
    spdlog::info(
        "CLod streaming: active branches {} (triangle={} voxel={}): pinned {} groups, unpinned {}, queued {}",
        branches, (branches & 1u) != 0u, (branches & 2u) != 0u, pinned, unpinned, queuedPinnedGroups);
}

void CLodStreamingSystem::SetGroupUsesPinnedStorage(uint32_t groupIndex, bool usesPinnedStorage) {
    if (usesPinnedStorage) {
        m_groupsUsingPinnedStorage.insert(groupIndex);
        return;
    }

    m_groupsUsingPinnedStorage.erase(groupIndex);
}

