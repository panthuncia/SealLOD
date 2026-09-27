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

    if (m_streamingDomainFullResetPending) {
        m_streamingDomainFullResetPending = false;
        RebuildStreamingDomainFromSnapshot(meshManager);
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
                setBitRange(m_streamingPinnedGroupsBitsCpu, pinnedRange.groupsBase, pinnedRange.groupCount, true);
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

void CLodStreamingSystem::SetGroupUsesPinnedStorage(uint32_t groupIndex, bool usesPinnedStorage) {
    if (usesPinnedStorage) {
        m_groupsUsingPinnedStorage.insert(groupIndex);
        return;
    }

    m_groupsUsingPinnedStorage.erase(groupIndex);
}

