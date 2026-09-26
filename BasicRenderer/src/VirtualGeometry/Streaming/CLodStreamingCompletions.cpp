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

void CLodStreamingSystem::ApplyDiskStreamingCompletions(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions");

    if (meshManager == nullptr) {
        return;
    }

    std::vector<br::render::CLodDiskStreamingCompletion> completions;
    {
        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::DrainCompletions");
        meshManager->DrainCompletedCLodDiskStreamingGroups(completions);
    }
    if (!m_readyStreamingCompletionRetryGroups.empty()) {
        completions.reserve(
            completions.size() +
            m_readyStreamingCompletionRetryGroups.size());
        for (uint32_t groupIndex : m_readyStreamingCompletionRetryGroups) {
            if (groupIndex <
                m_readyStreamingCompletionRetryQueuedByGroup.size()) {
                m_readyStreamingCompletionRetryQueuedByGroup[groupIndex] = 0u;
            }
            auto readyIt =
                m_readyStreamingCompletionsByGroup.find(groupIndex);
            if (readyIt == m_readyStreamingCompletionsByGroup.end()) {
                continue;
            }
            completions.push_back(std::move(readyIt->second));
            m_readyStreamingCompletionBytes -=
                std::min(
                    m_readyStreamingCompletionBytes,
                    CLodReadyCompletionStorageBytes(completions.back()));
            m_readyStreamingCompletionsByGroup.erase(readyIt);
        }
        m_readyStreamingCompletionRetryGroups.clear();
    }
    std::unordered_map<uint32_t, uint32_t> completionDepths;
    completionDepths.reserve(completions.size());
    for (const auto& completion : completions) {
        completionDepths.emplace(
            completion.groupGlobalIndex,
            SelectedAncestorDepth(
                completion.groupGlobalIndex, meshManager));
    }
    std::stable_sort(
        completions.begin(),
        completions.end(),
        [&completionDepths](const auto& lhs, const auto& rhs) {
            return completionDepths.at(lhs.groupGlobalIndex) <
                completionDepths.at(rhs.groupGlobalIndex);
        });

    {
        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ApplyCompletions");
        m_uploadStream->BeginBulkUpload();
        const bool recordCpuTiming =
            basic_telemetry::Enabled() && !completions.empty();
        const uint64_t applyLoopStartNs =
            recordCpuTiming ? basic_telemetry::NowNs() : 0u;
        uint64_t allocatePagesNs = 0u;
        uint64_t resolvePayloadsNs = 0u;
        for (uint32_t completionIndex = 0; completionIndex < static_cast<uint32_t>(completions.size()); ++completionIndex) {
            ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ApplyOne");
            auto& completion = completions[completionIndex];
            const uint32_t groupIndex = completion.groupGlobalIndex;
            ZoneValue(groupIndex);
            if (groupIndex >= m_streamingStorageGroupCapacity) {
                continue;
            }
            {
                ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::RecordCompletion");
                RecordStreamingCompletion(groupIndex, completion);
            }

            auto clearCompletionRequestState = [this, groupIndex]() {
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
            };

            auto preAllocIt = m_preAllocatedPagesByGroup.end();
            {
                ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::LookupPreallocation");
                preAllocIt = m_preAllocatedPagesByGroup.find(groupIndex);
            }

            if (completion.success) {
                bool groupActive = false;
                bool requestStateValid = false;
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ValidateRequestState");
                    groupActive = IsGroupActive(groupIndex);
                    requestStateValid =
                        groupIndex < m_streamingRequestStateByGroup.size() &&
                        m_streamingRequestStateByGroup[groupIndex] ==
                            StreamingRequestState::DiskIo;
                }
                if (!groupActive) {
                    if (preAllocIt != m_preAllocatedPagesByGroup.end()) {
                        ReleasePreAllocatedPages(preAllocIt->second, meshManager);
                        m_preAllocatedPagesByGroup.erase(preAllocIt);
                    }
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }
                if (!requestStateValid) {
                    if (preAllocIt != m_preAllocatedPagesByGroup.end()) {
                        ReleasePreAllocatedPages(preAllocIt->second, meshManager);
                        m_preAllocatedPagesByGroup.erase(preAllocIt);
                    }
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

                // A child must not consume its page credits while its selected
                // fallback parent is still waiting for pages. Holding both
                // allocations can prevent the parent from ever becoming
                // resident, which in turn permanently blocks child promotion.
                // Preallocated requests may already have GPU writes targeting
                // their pages, so only park completions that have not acquired
                // physical storage yet.
                const bool selectedParentResident =
                    IsGroupSelectedParentResident(
                        groupIndex, meshManager);
                const bool selectedParentAvailable =
                    selectedParentResident ||
                    IsGroupSelectedParentResidentOrCommitReady(
                        groupIndex, meshManager);
                if (preAllocIt == m_preAllocatedPagesByGroup.end() &&
                    !selectedParentAvailable) {
                    uint32_t parentGroup = 0u;
                    if (meshManager->TryGetCLodParentGroup(
                            groupIndex, parentGroup)) {
                        ParkReadyCompletionForParent(
                            groupIndex,
                            parentGroup,
                            std::move(completion));
                        m_pendingResidencyCommitGroups.erase(groupIndex);
                        continue;
                    }
                }
                if (preAllocIt == m_preAllocatedPagesByGroup.end() &&
                    !selectedParentResident &&
                    selectedParentAvailable) {
                    ++m_transactionalChildCompletionAdmissions;
                }

                // Prevent late allocation for this completion from choosing a
                // page owned by its resident fallback chain.
                ProtectGroupAndAncestors(groupIndex);

                PreAllocatedPages preAlloc{};
                const bool hadPreAllocation = preAllocIt != m_preAllocatedPagesByGroup.end();
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::TakePreallocation");
                    if (hadPreAllocation) {
                        preAlloc = std::move(preAllocIt->second);
                        m_preAllocatedPagesByGroup.erase(preAllocIt);
                    }
                }
                uint32_t expectedPageCount = preAlloc.segmentCount;
                if (!hadPreAllocation) {
                    expectedPageCount = static_cast<uint32_t>(
                        completion.meshPageIndices.size());
                    if (expectedPageCount > 0u) {
                        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::AllocatePagesAfterReadFallback");
                        std::vector<uint32_t> completionPageSizes(expectedPageCount, 0u);
                        for (uint32_t pageIndex = 0u; pageIndex < expectedPageCount; ++pageIndex) {
                            if (pageIndex < completion.mappedPageBlobSizes.size()) {
                                completionPageSizes[pageIndex] =
                                    completion.mappedPageBlobSizes[pageIndex];
                            } else if (pageIndex < completion.pageBlobs.size()) {
                                completionPageSizes[pageIndex] =
                                    static_cast<uint32_t>(completion.pageBlobs[pageIndex].size());
                            }
                        }
                        const uint64_t allocateStartNs =
                            recordCpuTiming
                                ? basic_telemetry::NowNs()
                                : 0u;
                        preAlloc = PreAllocatePagesForGroup(
                            groupIndex,
                            completion.groupsBase,
                            std::span<const uint32_t>(
                                completion.meshPageIndices.data(),
                                completion.meshPageIndices.size()),
                            std::span<const uint32_t>(
                                completionPageSizes.data(),
                                completionPageSizes.size()),
                            meshManager);
                        if (recordCpuTiming) {
                            allocatePagesNs +=
                                basic_telemetry::NowNs() -
                                allocateStartNs;
                        }
                        preAlloc.requestGeneration = groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
                            ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
                            : 0u;
                        if (preAlloc.segmentCount == 0u) {
                            if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
                                ++m_streamingDiagnosticsByGroup[groupIndex].preallocationDeferrals;
                            }
                            ++m_streamingDiagnosticsPreallocationDeferralsThisFrame;
                            const uint32_t wordAddress = BitWordAddress(groupIndex);
                            const uint32_t bitMask = BitMask(groupIndex);
                            if (wordAddress < m_streamingPinnedGroupsBitsCpu.size() &&
                                (m_streamingPinnedGroupsBitsCpu[wordAddress] & bitMask) != 0u) {
                                m_streamingResidencyInitializedBitsCpu[wordAddress] &= ~bitMask;
                            }
                            ParkReadyCompletionForPageCredit(
                                groupIndex, std::move(completion));
                            m_pendingResidencyCommitGroups.erase(groupIndex);
                            continue;
                        }
                    }
                }

                if (preAlloc.segmentCount != expectedPageCount) {
                    spdlog::warn(
                        "CLod streaming: dropping successful IO completion for group {} because allocated page count {} does not match expected {}",
                        groupIndex,
                        preAlloc.segmentCount,
                        expectedPageCount);
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

                bool waitsForPendingSharedPage = false;
                uint32_t pendingSharedPage = UINT32_MAX;
                uint64_t pendingSharedKey = kInvalidCLodMeshPageKey;
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::CheckSharedPageWaits");
                    for (uint32_t seg = 0; seg < expectedPageCount; ++seg) {
                    const bool reusedPage =
                        seg < static_cast<uint32_t>(preAlloc.segmentNeedsFetch.size()) &&
                        !preAlloc.segmentNeedsFetch[seg];
                    if (!reusedPage) {
                        continue;
                    }

                    const uint32_t page = seg < static_cast<uint32_t>(preAlloc.pagesBySegment.size())
                        ? preAlloc.pagesBySegment[seg]
                        : ~0u;
                    const uint64_t key = seg < static_cast<uint32_t>(preAlloc.meshPageKeys.size())
                        ? preAlloc.meshPageKeys[seg]
                        : kInvalidCLodMeshPageKey;
                    if (!IsPhysicalPageResidentForKey(page, key) &&
                        IsPhysicalPagePendingForKey(page, key)) {
                        waitsForPendingSharedPage = true;
                        pendingSharedPage = page;
                        pendingSharedKey = key;
                        break;
                    }
                }
                }
                if (waitsForPendingSharedPage) {
                    m_preAllocatedPagesByGroup[groupIndex] = std::move(preAlloc);
                    ParkReadyCompletionForSharedPage(
                        groupIndex,
                        pendingSharedPage,
                        pendingSharedKey,
                        std::move(completion));
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    continue;
                }

                completion.segmentNeedsFetch = preAlloc.segmentNeedsFetch;
                completion.preAllocatedPages = preAlloc.pagesBySegment;
                const bool payloadGpuReady =
                    completion.payloadKind == br::render::CLodDiskStreamingPayloadKind::GpuPagesReady;
                const bool payloadUsesExistingPages =
                    completion.payloadKind == br::render::CLodDiskStreamingPayloadKind::ReusedExistingPages;
                const bool payloadNeedsCpuUpload =
                    completion.payloadKind == br::render::CLodDiskStreamingPayloadKind::CpuPageBlobs;
                const bool payloadUsesMappedViews =
                    completion.payloadKind ==
                    br::render::CLodDiskStreamingPayloadKind::
                        CpuMappedPageViews;

                // Validate the preallocation immediately before using it. A
                // delayed I/O completion can arrive after one of its physical
                // pages was retired and reused. Previously the CPU upload was
                // queued first and AssignPagesToGroup detected the mismatch
                // afterwards, so stale data could overwrite the page's new
                // mesh owner even though the completion was ultimately rejected.
                bool pageOwnershipValid = true;
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ValidatePageOwnership");
                    for (uint32_t seg = 0; seg < expectedPageCount; ++seg) {
                    const uint32_t page = preAlloc.pagesBySegment[seg];
                    const uint64_t meshPageKey = preAlloc.meshPageKeys[seg];
                    const bool fetchedPage = preAlloc.segmentNeedsFetch[seg];
                    const bool ownerMatches =
                        page != ~0u &&
                        page < m_pageOwnerMeshPageKey.size() &&
                        meshPageKey != kInvalidCLodMeshPageKey &&
                        m_pageOwnerMeshPageKey[page] == meshPageKey;
                    const bool stateMatches = fetchedPage
                        ? page < m_pendingPageOwnerGroup.size() &&
                            m_pendingPageOwnerGroup[page] == groupIndex
                        : IsPhysicalPageResidentForKey(page, meshPageKey) ||
                            IsPhysicalPagePendingForKey(page, meshPageKey);
                    if (!ownerMatches || !stateMatches) {
                        spdlog::warn(
                            "CLod streaming: dropping stale completion for group {} seg {} page {} key {} before upload (ownerKey={}, fetched={}, pendingOwner={})",
                            groupIndex,
                            seg,
                            page,
                            meshPageKey,
                            page < m_pageOwnerMeshPageKey.size() ? m_pageOwnerMeshPageKey[page] : kInvalidCLodMeshPageKey,
                            fetchedPage,
                            page < m_pendingPageOwnerGroup.size() ? m_pendingPageOwnerGroup[page] : UINT32_MAX);
                        pageOwnershipValid = false;
                        break;
                    }
                }
                }
                if (!pageOwnershipValid) {
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::PrepareRenderMetadata");
                if (payloadGpuReady) {
                    if (completion.pageAllocations.size() != expectedPageCount ||
                        completion.pageMapEntries.size() != expectedPageCount ||
                        completion.preAllocatedPages.size() != expectedPageCount) {
                        spdlog::warn(
                            "CLod streaming: dropping DirectStorage completion for group {} because ready GPU payload has invalid render metadata (allocations={}, pageMapEntries={}, preAllocated={}, expected={})",
                            groupIndex,
                            completion.pageAllocations.size(),
                            completion.pageMapEntries.size(),
                            completion.preAllocatedPages.size(),
                            expectedPageCount);
                        ReleasePreAllocatedPages(preAlloc, meshManager);
                        m_pendingResidencyCommitGroups.erase(groupIndex);
                        clearCompletionRequestState();
                        continue;
                    }
                }
                else {
                    completion.pageAllocations.resize(expectedPageCount);
                    completion.pageMapEntries.resize(expectedPageCount);
                }
                }

                PagePool* pool = meshManager->GetCLodPagePool();
                bool payloadValid = true;
                bool queuedPayloadUpload = false;
                uint64_t queuedPayloadBytes = 0u;
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ResolveAndQueuePayloads");
                    ZoneValue(expectedPageCount);
                    const uint64_t resolveStartNs =
                        recordCpuTiming
                            ? basic_telemetry::NowNs()
                            : 0u;
                    for (uint32_t seg = 0; seg < expectedPageCount; ++seg) {
                    const uint32_t page = preAlloc.pagesBySegment[seg];
                    const size_t pageSize =
                        pool != nullptr ? pool->GetPageSize(page) : 0u;
                    PagePool::PageAllocation allocation{ page, 1u };
                    if (!payloadGpuReady) {
                        completion.pageAllocations[seg] = allocation;
                    }
                    const bool needsFetch = seg < preAlloc.segmentNeedsFetch.size() && preAlloc.segmentNeedsFetch[seg];
                    if (needsFetch &&
                        (payloadNeedsCpuUpload ||
                            payloadUsesMappedViews)) {
                        std::span<const std::byte> payload;
                        if (payloadNeedsCpuUpload &&
                            seg < completion.pageBlobs.size()) {
                            payload = std::span<const std::byte>(
                                completion.pageBlobs[seg].data(),
                                completion.pageBlobs[seg].size());
                        }
                        else if (payloadUsesMappedViews &&
                            completion.mappedContainer != nullptr &&
                            seg < completion.mappedPageBlobSizes.size() &&
                            seg < completion.mappedPageBlobOffsets.size()) {
                            completion.mappedContainer->GetBlob(
                                completion.mappedPageBlobOffsets[seg],
                                completion.mappedPageBlobSizes[seg],
                                payload);
                        }
                        if (pool == nullptr ||
                            payload.empty() ||
                            payload.size() > pageSize) {
                            spdlog::warn(
                                "CLod streaming: dropping completion for group {} because segment {} has invalid page payload",
                                groupIndex,
                                seg);
                            payloadValid = false;
                            break;
                        }
                        const uint64_t meshPageKey = seg < preAlloc.meshPageKeys.size()
                            ? preAlloc.meshPageKeys[seg]
                            : kInvalidCLodMeshPageKey;
                        LogPageOverwriteInvariant(page, groupIndex, seg, meshPageKey, "cpu-page-upload");
                        pool->UploadToPage(
                            page, 0, payload.data(), payload.size());
                        queuedPayloadBytes +=
                            static_cast<uint64_t>(payload.size());
                        queuedPayloadUpload = true;
                    }
                    else if (needsFetch && !payloadGpuReady && !payloadUsesExistingPages) {
                        spdlog::warn(
                            "CLod streaming: dropping completion for group {} because segment {} needs fetch but payload kind is invalid",
                            groupIndex,
                            seg);
                        payloadValid = false;
                        break;
                    }
                    if (!payloadGpuReady) {
                        completion.pageMapEntries[seg].slabDescriptorIndex = pool != nullptr ? pool->GetSlabDescriptorIndex(allocation) : 0u;
                        completion.pageMapEntries[seg].slabByteOffset = pool != nullptr ? static_cast<uint32_t>(pool->PageToSlabByteOffset(page)) : 0u;
                    }
                }
                if (recordCpuTiming) {
                    resolvePayloadsNs +=
                        basic_telemetry::NowNs() -
                        resolveStartNs;
                }
                }
                if (!payloadValid) {
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }
                if (queuedPayloadUpload) {
                    RecordStreamingUploadQueued(
                        groupIndex,
                        queuedPayloadBytes);
                }
                if ((!queuedPayloadUpload || payloadGpuReady) && completion.fetchedPageCount != 0u) {
                    RecordStreamingUploadQueued(groupIndex, completion.totalStreamedBytes);
                }

                bool renderableCompletionValid = false;
                {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ValidateRenderableCompletion");
                    renderableCompletionValid = ValidateRenderableCompletion(
                        groupIndex,
                        preAlloc,
                        completion,
                        expectedPageCount,
                        meshManager);
                }
                if (!renderableCompletionValid) {
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

                if (expectedPageCount > 0u) {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::AssignPagesToGroup");
                    if (!AssignPagesToGroup(groupIndex, preAlloc, meshManager)) {
                        ReleasePreAllocatedPages(preAlloc, meshManager);
                        m_pendingResidencyCommitGroups.erase(groupIndex);
                        clearCompletionRequestState();
                        continue;
                    }
                }
                else {
                    if (m_groupOwnedPages.find(groupIndex) != m_groupOwnedPages.end()) {
                        ReleaseGroupResidency(groupIndex, meshManager, true);
                    }
                    m_groupOwnedPages[groupIndex] = {};
                    m_groupOwnedMeshPageKeys[groupIndex] = {};
                    m_groupCommittedPageMaps.erase(groupIndex);
                    SetGroupUsesPinnedStorage(groupIndex, IsGroupPinned(groupIndex));
                }

                {
                ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::TouchAndCommitResidency");
                TouchGroupPages(groupIndex);

                const bool committed = meshManager->CommitCLodGroupResidency(
                    groupIndex,
                    completion.chunk,
                    std::span<const uint32_t>(completion.meshPageIndices.data(), completion.meshPageIndices.size()),
                    std::span<const GroupPageMapEntry>(completion.pageMapEntries.data(), completion.pageMapEntries.size()),
                    std::span<const PagePool::PageAllocation>(completion.pageAllocations.data(), completion.pageAllocations.size()),
                    completion.totalStreamedBytes);
                if (committed) {
                    auto& committedMap = m_groupCommittedPageMaps[groupIndex];
                    committedMap.pageAllocations = completion.pageAllocations;
                    committedMap.pageMapEntries = completion.pageMapEntries;
                    committedMap.commitTick = m_streamingDiagnosticTick;
                    InstallPrefetchedChildGroupLayouts(groupIndex, std::move(completion.prefetchedChildLayouts));
                    m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                    m_pendingResidencyCommitGroups.insert(groupIndex);
                    m_residencyGroupsAwaitingUploadFence.push_back(groupIndex);
                    RecordStreamingCommitQueued(groupIndex);
                    WakeReadyCompletionsForParent(
                        groupIndex, &completions);
                } else {
                    ReleaseGroupResidency(groupIndex, meshManager, true);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                }
                }
            }
            else {
                ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::HandleFailedCompletion");
                m_pendingResidencyCommitGroups.erase(groupIndex);
                m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                if (preAllocIt != m_preAllocatedPagesByGroup.end()) {
                    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ReleaseFailedPreallocation");
                    ReleasePreAllocatedPages(preAllocIt->second, meshManager);
                    m_preAllocatedPagesByGroup.erase(preAllocIt);
                }

                const uint32_t wordAddress = BitWordAddress(groupIndex);
                const uint32_t bitMask = BitMask(groupIndex);
                if (wordAddress < m_streamingPinnedGroupsBitsCpu.size() &&
                    (m_streamingPinnedGroupsBitsCpu[wordAddress] & bitMask) != 0u) {
                    m_streamingResidencyInitializedBitsCpu[wordAddress] &= ~bitMask;
                }
            }

            {
            ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::FinalizeRequestState");
            if (m_pendingResidencyCommitGroups.find(groupIndex) == m_pendingResidencyCommitGroups.end()) {
                clearCompletionRequestState();
            }
            }
        }
        const uint64_t stagePayloadsStartNs =
            recordCpuTiming ? basic_telemetry::NowNs() : 0u;
        m_uploadStream->EndBulkUpload();
        const uint64_t stagePayloadsNs = recordCpuTiming
            ? basic_telemetry::NowNs() - stagePayloadsStartNs
            : 0u;
        if (recordCpuTiming) {
            const uint64_t applyLoopNs =
                basic_telemetry::NowNs() - applyLoopStartNs;
            basic_telemetry::Record(
                "CLod.ApplyCompletions",
                applyLoopNs);
            basic_telemetry::Record(
                "CLod.ApplyCompletions.AllocatePages",
                allocatePagesNs);
            basic_telemetry::Record(
                "CLod.ApplyCompletions.ResolvePayloads",
                resolvePayloadsNs);
            if (stagePayloadsNs != 0u) {
                basic_telemetry::Record(
                    "CLod.ApplyCompletions.StagePayloads",
                    stagePayloadsNs);
            }
            basic_telemetry::AddCounter(
                "CLod.ApplyCompletions.Processed",
                completions.size());
        }
    }
}
