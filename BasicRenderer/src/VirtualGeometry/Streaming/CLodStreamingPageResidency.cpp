#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"
#include "VirtualGeometry/Streaming/CLodStreamingInternals.h"
#include "VirtualGeometry/Streaming/CLodStreamingTraceInternals.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>
#include <nlohmann/json.hpp>

#include "Runtime/Device/DeviceManager.h"
#include "Runtime/Settings/SettingsManager.h"
#include "Scene/Views/ViewManager.h"
#include "VirtualGeometry/Streaming/RenderPasses/CLodStreamingBeginFramePass.h"
#include "VirtualGeometry/Streaming/RenderPasses/CLodStreamingFeedbackSortPass.h"
#include "VirtualGeometry/Streaming/CLodStreamingReadbackSources.h"
#include "VirtualGeometry/Streaming/RenderPasses/CLodDirectStorageLaunchPass.h"
#include "VirtualGeometry/Streaming/Publication/CLodResidencyStorageArtifacts.h"
#include "BasicRenderer/Extensions/RenderContext.h"
#include "Render/Runtime/ExternalSignalReservation.h"
#include "Render/Runtime/UploadTypes.h"
#include "Managers/UploadInstance.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "RenderPasses/StreamingUploadPass.h"
#include "Runtime/GraphIntegration/Resolvers/ResourceGroupResolver.h"
#include "BasicRenderer/Extensions/Buffers/DynamicBuffer.h"
#include "Resources/BackedResource.h"
#include "Utilities/Utilities.h"
#include <BasicRenderer/Diagnostics/NvPerfIntegration.h>
#include <BasicTelemetry/Telemetry.h>
#include "BasicRenderer/Assets/ClusterLODShaderTypes.h"
#include "BuiltinResources.h"

namespace {
    uint64_t MakeCLodMeshPageKey(uint32_t groupsBase, uint32_t meshPageIndex) {
        return (static_cast<uint64_t>(groupsBase) << 32ull) | static_cast<uint64_t>(meshPageIndex);
    }

    bool CLodTrianglePageHasSourceGroup(std::span<const std::byte> blob, uint32_t sourceGroupLocalIndex) {
        if (blob.size() < sizeof(CLodPageHeader)) {
            return true;
        }

        CLodPageHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        if (header.descriptorOffset == 0u ||
            header.meshletCount == 0u ||
            header.descriptorOffset > blob.size()) {
            return true;
        }

        const size_t descriptorBytes =
            static_cast<size_t>(header.meshletCount) * sizeof(CLodMeshletDescriptor);
        if (static_cast<size_t>(header.descriptorOffset) + descriptorBytes > blob.size()) {
            return true;
        }

        for (uint32_t meshletIndex = 0; meshletIndex < header.meshletCount; ++meshletIndex) {
            CLodMeshletDescriptor descriptor{};
            std::memcpy(
                &descriptor,
                blob.data() + header.descriptorOffset + static_cast<size_t>(meshletIndex) * sizeof(CLodMeshletDescriptor),
                sizeof(descriptor));
            if (descriptor.sourceGroupLocalIndex == sourceGroupLocalIndex) {
                return true;
            }
        }
        return false;
    }

    bool ReadCLodTrianglePageDescriptor(
        std::span<const std::byte> blob,
        const CLodPageHeader& header,
        uint32_t meshletIndex,
        CLodMeshletDescriptor& outDescriptor) {
        const size_t descriptorOffset =
            static_cast<size_t>(header.descriptorOffset) +
            static_cast<size_t>(meshletIndex) * sizeof(CLodMeshletDescriptor);
        if (descriptorOffset + sizeof(CLodMeshletDescriptor) > blob.size()) {
            return false;
        }

        std::memcpy(&outDescriptor, blob.data() + descriptorOffset, sizeof(outDescriptor));
        return true;
    }

    bool ValidateCLodTrianglePageSegmentSourceGroups(
        std::span<const std::byte> blob,
        uint32_t expectedLocalGroup,
        uint32_t meshPageIndex,
        const br::render::CLodGroupStreamingInfo& info,
        uint32_t groupIndex,
        uint32_t completionSegmentIndex,
        uint32_t physicalPage,
        const GroupPageMapEntry& pageMapEntry) {
        if (blob.size() < sizeof(CLodPageHeader)) {
            return true;
        }

        CLodPageHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        if (header.descriptorOffset == 0u ||
            header.meshletCount == 0u ||
            header.descriptorOffset > blob.size()) {
            return true;
        }

        const size_t descriptorBytes =
            static_cast<size_t>(header.meshletCount) * sizeof(CLodMeshletDescriptor);
        if (static_cast<size_t>(header.descriptorOffset) + descriptorBytes > blob.size()) {
            return true;
        }

        bool foundSegmentForPage = false;
        bool allExpected = true;
        uint32_t loggedMismatches = 0u;
        for (uint32_t localSegmentIndex = 0u;
            localSegmentIndex < static_cast<uint32_t>(info.segments.size());
            ++localSegmentIndex) {
            const ClusterLODGroupSegment& segment = info.segments[localSegmentIndex];
            if (segment.meshletCount == 0u || segment.pageIndex < info.group.pageMapBase) {
                continue;
            }
            const uint32_t localPageIndex = segment.pageIndex - info.group.pageMapBase;
            if (localPageIndex >= static_cast<uint32_t>(info.meshPageIndices.size()) ||
                info.meshPageIndices[localPageIndex] != meshPageIndex) {
                continue;
            }

            foundSegmentForPage = true;
            const uint64_t endMeshlet =
                static_cast<uint64_t>(segment.firstMeshletInPage) +
                static_cast<uint64_t>(segment.meshletCount);
            if (endMeshlet > header.meshletCount) {
                spdlog::error(
                    "CLod streaming: fetched page segment range is outside payload for group {} localGroup={} completionSeg={} meshPage={} physicalPage={} segment={} firstMeshlet={} meshletCount={} payloadMeshletCount={} slabMap={}:{}",
                    groupIndex,
                    expectedLocalGroup,
                    completionSegmentIndex,
                    meshPageIndex,
                    physicalPage,
                    info.group.firstSegment + localSegmentIndex,
                    segment.firstMeshletInPage,
                    segment.meshletCount,
                    header.meshletCount,
                    pageMapEntry.slabDescriptorIndex,
                    pageMapEntry.slabByteOffset);
                allExpected = false;
                continue;
            }

            for (uint32_t meshletOffset = 0u; meshletOffset < segment.meshletCount; ++meshletOffset) {
                const uint32_t pageLocalMeshlet = segment.firstMeshletInPage + meshletOffset;
                CLodMeshletDescriptor descriptor{};
                if (!ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet, descriptor)) {
                    allExpected = false;
                    continue;
                }

                if (descriptor.sourceGroupLocalIndex == expectedLocalGroup) {
                    continue;
                }

                allExpected = false;
                if (loggedMismatches < 4u) {
                    CLodMeshletDescriptor previousDescriptor{};
                    CLodMeshletDescriptor nextDescriptor{};
                    const uint32_t previousSourceGroup =
                        pageLocalMeshlet > 0u &&
                            ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet - 1u, previousDescriptor)
                        ? previousDescriptor.sourceGroupLocalIndex
                        : UINT32_MAX;
                    const uint32_t nextSourceGroup =
                        pageLocalMeshlet + 1u < header.meshletCount &&
                            ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet + 1u, nextDescriptor)
                        ? nextDescriptor.sourceGroupLocalIndex
                        : UINT32_MAX;
                    spdlog::error(
                        "CLod streaming: fetched page segment source mismatch for group {} localGroup={} completionSeg={} meshPage={} physicalPage={} segment={} expectedMeshlets=[{}, {}) pageLocalMeshlet={} foundLocalGroup={} neighborLocalGroups=[{}, {}] payloadMeshletCount={} slabMap={}:{}",
                        groupIndex,
                        expectedLocalGroup,
                        completionSegmentIndex,
                        meshPageIndex,
                        physicalPage,
                        info.group.firstSegment + localSegmentIndex,
                        segment.firstMeshletInPage,
                        segment.firstMeshletInPage + segment.meshletCount,
                        pageLocalMeshlet,
                        descriptor.sourceGroupLocalIndex,
                        previousSourceGroup,
                        nextSourceGroup,
                        header.meshletCount,
                        pageMapEntry.slabDescriptorIndex,
                        pageMapEntry.slabByteOffset);
                    ++loggedMismatches;
                }
            }
        }

        if (!foundSegmentForPage) {
            spdlog::warn(
                "CLod streaming: fetched page for group {} localGroup={} completionSeg={} meshPage={} physicalPage={} had no matching group segment; groupSegmentRange=[{}, {}) groupPageMapBase={} groupPageCount={} slabMap={}:{}",
                groupIndex,
                expectedLocalGroup,
                completionSegmentIndex,
                meshPageIndex,
                physicalPage,
                info.group.firstSegment,
                info.group.firstSegment + info.group.segmentCount,
                info.group.pageMapBase,
                info.group.pageCount,
                pageMapEntry.slabDescriptorIndex,
                pageMapEntry.slabByteOffset);
        }

        return allExpected;
    }

    bool ValidateCLodTrianglePageAllReferencedSegmentSourceGroups(
        std::span<const std::byte> blob,
        uint32_t meshPageIndex,
        const br::render::CLodGroupStreamingInfo& info,
        uint32_t requestingGroupIndex,
        uint32_t completionSegmentIndex,
        uint32_t physicalPage,
        const GroupPageMapEntry& pageMapEntry) {
        if (blob.size() < sizeof(CLodPageHeader)) {
            return true;
        }

        CLodPageHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        if (header.descriptorOffset == 0u ||
            header.meshletCount == 0u ||
            header.descriptorOffset > blob.size()) {
            return true;
        }

        const size_t descriptorBytes =
            static_cast<size_t>(header.meshletCount) * sizeof(CLodMeshletDescriptor);
        if (static_cast<size_t>(header.descriptorOffset) + descriptorBytes > blob.size()) {
            return true;
        }

        bool foundReferencedSegmentForPage = false;
        bool allExpected = true;
        uint32_t loggedMismatches = 0u;
        for (const br::render::CLodGroupStreamingInfo::ReferencedPageSegment& referencedSegment
            : info.referencedPageSegments) {
            const ClusterLODGroupSegment& segment = referencedSegment.segment;
            if (referencedSegment.meshPageIndex != meshPageIndex || segment.meshletCount == 0u) {
                continue;
            }

            foundReferencedSegmentForPage = true;
            const uint64_t endMeshlet =
                static_cast<uint64_t>(segment.firstMeshletInPage) +
                static_cast<uint64_t>(segment.meshletCount);
            if (endMeshlet > header.meshletCount) {
                spdlog::error(
                    "CLod streaming: fetched page referenced-segment range is outside payload for requestingGroup={} referencedGroup={} referencedLocalGroup={} completionSeg={} meshPage={} physicalPage={} segment={} firstMeshlet={} meshletCount={} payloadMeshletCount={} slabMap={}:{}",
                    requestingGroupIndex,
                    referencedSegment.sourceGroupGlobalIndex,
                    referencedSegment.sourceGroupLocalIndex,
                    completionSegmentIndex,
                    meshPageIndex,
                    physicalPage,
                    referencedSegment.segmentGlobalIndex,
                    segment.firstMeshletInPage,
                    segment.meshletCount,
                    header.meshletCount,
                    pageMapEntry.slabDescriptorIndex,
                    pageMapEntry.slabByteOffset);
                allExpected = false;
                continue;
            }

            for (uint32_t meshletOffset = 0u; meshletOffset < segment.meshletCount; ++meshletOffset) {
                const uint32_t pageLocalMeshlet = segment.firstMeshletInPage + meshletOffset;
                CLodMeshletDescriptor descriptor{};
                if (!ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet, descriptor)) {
                    allExpected = false;
                    continue;
                }

                if (descriptor.sourceGroupLocalIndex == referencedSegment.sourceGroupLocalIndex) {
                    continue;
                }

                allExpected = false;
                if (loggedMismatches < 12u) {
                    CLodMeshletDescriptor previousDescriptor{};
                    CLodMeshletDescriptor nextDescriptor{};
                    const uint32_t previousSourceGroup =
                        pageLocalMeshlet > 0u &&
                            ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet - 1u, previousDescriptor)
                        ? previousDescriptor.sourceGroupLocalIndex
                        : UINT32_MAX;
                    const uint32_t nextSourceGroup =
                        pageLocalMeshlet + 1u < header.meshletCount &&
                            ReadCLodTrianglePageDescriptor(blob, header, pageLocalMeshlet + 1u, nextDescriptor)
                        ? nextDescriptor.sourceGroupLocalIndex
                        : UINT32_MAX;
                    spdlog::error(
                        "CLod streaming: fetched page referenced-segment source mismatch for requestingGroup={} referencedGroup={} referencedLocalGroup={} completionSeg={} meshPage={} physicalPage={} segment={} expectedMeshlets=[{}, {}) pageLocalMeshlet={} foundLocalGroup={} neighborLocalGroups=[{}, {}] payloadMeshletCount={} slabMap={}:{}",
                        requestingGroupIndex,
                        referencedSegment.sourceGroupGlobalIndex,
                        referencedSegment.sourceGroupLocalIndex,
                        completionSegmentIndex,
                        meshPageIndex,
                        physicalPage,
                        referencedSegment.segmentGlobalIndex,
                        segment.firstMeshletInPage,
                        segment.firstMeshletInPage + segment.meshletCount,
                        pageLocalMeshlet,
                        descriptor.sourceGroupLocalIndex,
                        previousSourceGroup,
                        nextSourceGroup,
                        header.meshletCount,
                        pageMapEntry.slabDescriptorIndex,
                        pageMapEntry.slabByteOffset);
                    ++loggedMismatches;
                }
            }
        }

        if (!foundReferencedSegmentForPage) {
            spdlog::warn(
                "CLod streaming: fetched page for requestingGroup={} completionSeg={} meshPage={} physicalPage={} had no referenced segments in streaming info; referencedSegmentCount={} slabMap={}:{}",
                requestingGroupIndex,
                completionSegmentIndex,
                meshPageIndex,
                physicalPage,
                info.referencedPageSegments.size(),
                pageMapEntry.slabDescriptorIndex,
                pageMapEntry.slabByteOffset);
        }

        return allExpected;
    }

}

bool CLodStreamingSystem::IsPhysicalPageResidentForKey(uint32_t page, uint64_t key) const {
    if (page == ~0u ||
        key == kInvalidCLodMeshPageKey ||
        page >= m_pageState.size() ||
        page >= m_pageOwnerMeshPageKey.size() ||
        m_pageState[page] != CLodPhysicalPageState::Resident ||
        m_pageOwnerMeshPageKey[page] != key) {
        return false;
    }

    return true;
}

bool CLodStreamingSystem::IsPhysicalPagePendingForKey(uint32_t page, uint64_t key) const {
    if (page == ~0u ||
        key == kInvalidCLodMeshPageKey ||
        page >= m_pageState.size() ||
        page >= m_pageOwnerMeshPageKey.size() ||
        m_pageOwnerMeshPageKey[page] != key) {
        return false;
    }

    if (m_pageState[page] != CLodPhysicalPageState::PreAllocatedCpuUpload &&
        m_pageState[page] != CLodPhysicalPageState::PendingDirectStorageWrite) {
        return false;
    }

    const auto pendingIt = m_pendingMeshPageToPhysicalPage.find(key);
    return pendingIt != m_pendingMeshPageToPhysicalPage.end() && pendingIt->second == page;
}

uint32_t CLodStreamingSystem::GetPendingMeshPageRefCount(uint32_t page, uint64_t key) const {
    if (page == ~0u || key == kInvalidCLodMeshPageKey) {
        return 0u;
    }

    const auto pendingIt = m_pendingMeshPageToPhysicalPage.find(key);
    if (pendingIt == m_pendingMeshPageToPhysicalPage.end() || pendingIt->second != page) {
        return 0u;
    }

    const auto refIt = m_pendingMeshPageRefCounts.find(key);
    return refIt != m_pendingMeshPageRefCounts.end() ? refIt->second : 0u;
}

void CLodStreamingSystem::AddPendingMeshPageReference(uint32_t page, uint64_t key) {
    if (page == ~0u || key == kInvalidCLodMeshPageKey) {
        return;
    }

    m_pendingMeshPageToPhysicalPage[key] = page;
    ++m_pendingMeshPageRefCounts[key];
}

void CLodStreamingSystem::ReleasePendingMeshPageReference(uint32_t page, uint64_t key) {
    if (page == ~0u || key == kInvalidCLodMeshPageKey) {
        return;
    }

    const auto pendingIt = m_pendingMeshPageToPhysicalPage.find(key);
    if (pendingIt == m_pendingMeshPageToPhysicalPage.end() || pendingIt->second != page) {
        return;
    }

    auto refIt = m_pendingMeshPageRefCounts.find(key);
    if (refIt == m_pendingMeshPageRefCounts.end()) {
        return;
    }

    if (refIt->second > 1u) {
        --refIt->second;
        return;
    }

    m_pendingMeshPageRefCounts.erase(refIt);
    m_pendingMeshPageToPhysicalPage.erase(pendingIt);
}

bool CLodStreamingSystem::DoesGroupReferencePhysicalPage(uint32_t groupIndex, uint32_t page) const {
    const auto pagesIt = m_groupOwnedPages.find(groupIndex);
    if (pagesIt == m_groupOwnedPages.end()) {
        return false;
    }

    const auto& pages = pagesIt->second;
    return std::find(pages.begin(), pages.end(), page) != pages.end();
}

bool CLodStreamingSystem::DoesGroupReferencePageKey(uint32_t groupIndex, uint32_t page, uint64_t key) const {
    const auto pagesIt = m_groupOwnedPages.find(groupIndex);
    const auto keysIt = m_groupOwnedMeshPageKeys.find(groupIndex);
    if (pagesIt == m_groupOwnedPages.end() || keysIt == m_groupOwnedMeshPageKeys.end()) {
        return false;
    }

    const auto& pages = pagesIt->second;
    const auto& keys = keysIt->second;
    const uint32_t count = std::min(static_cast<uint32_t>(pages.size()), static_cast<uint32_t>(keys.size()));
    for (uint32_t seg = 0; seg < count; ++seg) {
        if (pages[seg] == page && keys[seg] == key) {
            return true;
        }
    }
    return false;
}

uint32_t CLodStreamingSystem::CountResidentGroupsForPageKey(uint32_t page, uint64_t key) const {
    if (page >= m_pageResidentGroups.size() || key == kInvalidCLodMeshPageKey) {
        return 0u;
    }

    uint32_t count = 0u;
    for (uint32_t groupIndex : m_pageResidentGroups[page]) {
        if (DoesGroupReferencePageKey(groupIndex, page, key)) {
            ++count;
        }
    }
    return count;
}

uint32_t CLodStreamingSystem::FindResidentGroupForPageKey(uint32_t page, uint64_t key) const {
    if (page >= m_pageResidentGroups.size() || key == kInvalidCLodMeshPageKey) {
        return ~0u;
    }

    for (uint32_t groupIndex : m_pageResidentGroups[page]) {
        if (DoesGroupReferencePageKey(groupIndex, page, key)) {
            return groupIndex;
        }
    }
    return ~0u;
}

uint32_t CLodStreamingSystem::ScrubStaleResidentGroups(uint32_t page) {
    if (page >= m_pageResidentGroups.size() || m_pageResidentGroups[page].empty()) {
        return 0u;
    }

    uint32_t removed = 0u;
    for (auto it = m_pageResidentGroups[page].begin(); it != m_pageResidentGroups[page].end();) {
        if (!DoesGroupReferencePhysicalPage(*it, page)) {
            it = m_pageResidentGroups[page].erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (removed != 0u) {
        spdlog::warn(
            "CLod streaming: scrubbed {} stale resident-group reference(s) from physical page {}",
            removed,
            page);
    }
    return removed;
}

bool CLodStreamingSystem::IsPhysicalPageRetired(uint32_t page) {
    if (!(page < m_pageState.size() &&
        m_pageState[page] == CLodPhysicalPageState::Retiring &&
        page < m_pageRetireAfterTick.size() &&
        m_streamingDiagnosticTick >= m_pageRetireAfterTick[page])) {
        return false;
    }

    if (!m_streamingUploadCompletionFenceHandle.IsValid()) {
        return true;
    }
    const uint64_t requiredEpoch = page < m_pageReuseRequiresNonResidentEpoch.size()
        ? m_pageReuseRequiresNonResidentEpoch[page]
        : 0u;
    if (requiredEpoch == 0u) {
        return true;
    }
    const uint64_t reuseFenceValue = page < m_pageReuseUploadFenceValue.size()
        ? m_pageReuseUploadFenceValue[page]
        : 0u;
    return reuseFenceValue != 0u &&
        m_streamingUploadCompletionFenceHandle.GetCompletedValue() >= reuseFenceValue;
}

bool CLodStreamingSystem::IsPhysicalPagePinnedStorage(uint32_t page) const {
    return page < m_pagePinnedStorage.size() && m_pagePinnedStorage[page] != 0u;
}

void CLodStreamingSystem::RetirePhysicalPage(uint32_t page, ICLodGeometryStorage* meshManager, bool pinned) {
    if (page == ~0u || page >= m_pageState.size()) {
        return;
    }
    if (page < m_pageOwnerMeshPageKey.size()) {
        WakeReadyCompletionsForPage(
            page, m_pageOwnerMeshPageKey[page]);
    }

    if (m_pageState[page] == CLodPhysicalPageState::Retiring) {
        if (page < m_pageRetireAfterTick.size()) {
            const uint64_t retireDelayTicks = static_cast<uint64_t>(m_streamingReadbackRingSize + 2u);
            m_pageRetireAfterTick[page] = std::max(m_pageRetireAfterTick[page], m_streamingDiagnosticTick + retireDelayTicks);
        }
        if (pinned && page < m_pageRetirePinned.size()) {
            m_pageRetirePinned[page] = 1u;
        }
        return;
    }

    const bool requiresNonResidentUpload =
        m_pageState[page] == CLodPhysicalPageState::Resident ||
        (page < m_pageResidentGroups.size() && !m_pageResidentGroups[page].empty());
    const uint64_t retiringKey = page < m_pageOwnerMeshPageKey.size()
        ? m_pageOwnerMeshPageKey[page]
        : kInvalidCLodMeshPageKey;
    if (retiringKey != kInvalidCLodMeshPageKey) {
        auto pendingIt = m_pendingMeshPageToPhysicalPage.find(retiringKey);
        if (pendingIt != m_pendingMeshPageToPhysicalPage.end() && pendingIt->second == page) {
            m_pendingMeshPageToPhysicalPage.erase(pendingIt);
        }
        auto pendingRefIt = m_pendingMeshPageRefCounts.find(retiringKey);
        if (pendingRefIt != m_pendingMeshPageRefCounts.end()) {
            m_pendingMeshPageRefCounts.erase(pendingRefIt);
        }
        auto residentIt = m_residentMeshPageToPhysicalPage.find(retiringKey);
        if (residentIt != m_residentMeshPageToPhysicalPage.end() && residentIt->second == page) {
            m_residentMeshPageToPhysicalPage.erase(residentIt);
        }
    }

    if (page < m_pageOwnerGroup.size()) {
        m_pageOwnerGroup[page] = -1;
        m_pageOwnerSegment[page] = 0u;
    }
    if (page < m_pendingPageOwnerGroup.size()) {
        m_pendingPageOwnerGroup[page] = ~0u;
        m_pendingPageOwnerSegment[page] = 0u;
    }
    if (page < m_pageOwnerMeshPageKey.size()) {
        m_pageOwnerMeshPageKey[page] = kInvalidCLodMeshPageKey;
    }
    if (page < m_pageResidentGroups.size()) {
        m_pageResidentGroups[page].clear();
    }

    m_pageState[page] = CLodPhysicalPageState::Retiring;
    m_retiringPhysicalPages.push_back(page);
    if (page < m_pageRetireAfterTick.size()) {
        const uint64_t retireDelayTicks = static_cast<uint64_t>(m_streamingReadbackRingSize + 2u);
        m_pageRetireAfterTick[page] = m_streamingDiagnosticTick + retireDelayTicks;
    }
    if (page < m_pageRetirePinned.size()) {
        m_pageRetirePinned[page] = pinned ? 1u : 0u;
    }
    if (page < m_pageReuseRequiresNonResidentEpoch.size()) {
        m_pageReuseRequiresNonResidentEpoch[page] = requiresNonResidentUpload
            ? m_streamingResidencyMutationEpoch
            : 0u;
    }
    if (page < m_pageReuseNonResidentQueuedTick.size()) {
        m_pageReuseNonResidentQueuedTick[page] = m_streamingNonResidentBitsQueuedTick;
    }
    if (page < m_pageReuseUploadFenceValue.size()) {
        const uint64_t requiredEpoch = page < m_pageReuseRequiresNonResidentEpoch.size()
            ? m_pageReuseRequiresNonResidentEpoch[page]
            : 0u;
        m_pageReuseUploadFenceValue[page] = requiredEpoch != 0u &&
            requiredEpoch <= m_streamingNonResidentBitsUploadFenceEpoch
            ? m_streamingNonResidentBitsUploadFenceValue
            : 0u;
        if (requiredEpoch != 0u && m_pageReuseUploadFenceValue[page] == 0u) {
            m_retiringPagesAwaitingUploadFence.push_back(page);
        }
    }
    spdlog::debug(
        "CLod streaming invariant: retired page {} pinned={} requiredNonResidentEpoch={} queuedEpoch={} queuedTick={} retireAfterTick={} currentTick={}",
        page,
        pinned,
        page < m_pageReuseRequiresNonResidentEpoch.size() ? m_pageReuseRequiresNonResidentEpoch[page] : 0u,
        m_streamingNonResidentBitsQueuedEpoch,
        page < m_pageReuseNonResidentQueuedTick.size() ? m_pageReuseNonResidentQueuedTick[page] : 0u,
        page < m_pageRetireAfterTick.size() ? m_pageRetireAfterTick[page] : 0u,
        m_streamingDiagnosticTick);
    PageLruForPage(page).Remove(page);
    (void)meshManager;
}

void CLodStreamingSystem::DrainRetiredPhysicalPages(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::DrainRetiredPhysicalPages");
    if (m_pageState.empty() || m_retiringPhysicalPages.empty()) {
        return;
    }

    TracyPlot("CLodStreaming.RetiringPhysicalPages", static_cast<int64_t>(m_retiringPhysicalPages.size()));
    std::vector<uint32_t> pinnedPagesToFree;
    uint32_t availablePageCredits = 0u;
    size_t pendingWriteIndex = 0u;
    for (uint32_t page : m_retiringPhysicalPages) {
        if (!IsPhysicalPageRetired(page)) {
            m_retiringPhysicalPages[pendingWriteIndex++] = page;
            continue;
        }

        const bool pinned = page < m_pageRetirePinned.size() && m_pageRetirePinned[page] != 0u;
        m_pageState[page] = CLodPhysicalPageState::Free;
        if (page < m_pageRetireAfterTick.size()) {
            m_pageRetireAfterTick[page] = 0u;
        }
        if (page < m_pageRetirePinned.size()) {
            m_pageRetirePinned[page] = 0u;
        }
        if (page < m_pageReuseUploadFenceValue.size()) {
            m_pageReuseUploadFenceValue[page] = 0u;
        }
        if (page < m_pageReuseRequiresNonResidentEpoch.size() &&
            m_pageReuseRequiresNonResidentEpoch[page] != 0u &&
            m_pageReuseRequiresNonResidentEpoch[page] <= m_streamingNonResidentBitsQueuedEpoch &&
            page < m_pageReuseNonResidentQueuedTick.size() &&
            m_pageReuseNonResidentQueuedTick[page] == 0u) {
            m_pageReuseNonResidentQueuedTick[page] = m_streamingNonResidentBitsQueuedTick;
        }

        LogPageOverwriteInvariant(page, ~0u, 0u, kInvalidCLodMeshPageKey, "retired-page-becoming-free");

        if (pinned) {
            pinnedPagesToFree.push_back(page);
            if (page < m_pagePinnedStorage.size()) {
                m_pagePinnedStorage[page] = 0u;
            }
        } else {
            PageLruForPage(page).Insert(page);
            ++availablePageCredits;
        }
    }
    m_retiringPhysicalPages.resize(pendingWriteIndex);
    WakeReadyPageCreditWaiters(availablePageCredits);

    if (!pinnedPagesToFree.empty() && meshManager != nullptr) {
        if (PagePool* pool = meshManager->GetCLodPagePool()) {
            pool->FreePinnedPages(pinnedPagesToFree);
        }
    }
}

void CLodStreamingSystem::ReleaseGroupResidency(uint32_t groupIndex, ICLodGeometryStorage* meshManager, bool clearPageMapEntries) {
    EvictPrefetchedChildLayoutsForOwner(groupIndex);
    m_pendingResidencyCommitGroups.erase(groupIndex);
    m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
    m_groupCommittedPageMaps.erase(groupIndex);

    auto pagesIt = m_groupOwnedPages.find(groupIndex);
    auto keysIt = m_groupOwnedMeshPageKeys.find(groupIndex);
    if (pagesIt == m_groupOwnedPages.end()) {
        SetGroupUsesPinnedStorage(groupIndex, false);
        return;
    }

    for (uint32_t slot = 0; slot < static_cast<uint32_t>(pagesIt->second.size()); ++slot) {
        const uint32_t page = pagesIt->second[slot];
        if (page == ~0u) {
            continue;
        }

        const uint64_t key = keysIt != m_groupOwnedMeshPageKeys.end() && slot < keysIt->second.size()
            ? keysIt->second[slot]
            : kInvalidCLodMeshPageKey;
        const bool wasResidentGroup = page < m_pageResidentGroups.size() &&
            m_pageResidentGroups[page].find(groupIndex) != m_pageResidentGroups[page].end();
        const bool hadPendingReference = !wasResidentGroup && GetPendingMeshPageRefCount(page, key) != 0u;
        if (hadPendingReference) {
            ReleasePendingMeshPageReference(page, key);
        }
        if (page < m_pageResidentGroups.size()) {
            m_pageResidentGroups[page].erase(groupIndex);
        }

        if (key != kInvalidCLodMeshPageKey) {
            auto refIt = m_residentMeshPageRefCounts.find(key);
            if (refIt != m_residentMeshPageRefCounts.end()) {
                const uint32_t committedRefsAfterRelease = CountResidentGroupsForPageKey(page, key);
                if (committedRefsAfterRelease != 0u) {
                    refIt->second = committedRefsAfterRelease;
                } else {
                    m_residentMeshPageRefCounts.erase(refIt);
                    auto residentIt = m_residentMeshPageToPhysicalPage.find(key);
                    if (residentIt != m_residentMeshPageToPhysicalPage.end() && residentIt->second == page) {
                        m_residentMeshPageToPhysicalPage.erase(residentIt);
                    }
                }
            }
        }

        const bool pageStillResident = key != kInvalidCLodMeshPageKey && CountResidentGroupsForPageKey(page, key) != 0u;
        if (pageStillResident) {
            if (page < m_pageOwnerGroup.size()) {
                const uint32_t nextOwner = FindResidentGroupForPageKey(page, key);
                if (nextOwner != ~0u) {
                    m_pageOwnerGroup[page] = static_cast<int32_t>(nextOwner);
                    auto nextPagesIt = m_groupOwnedPages.find(nextOwner);
                    auto nextKeysIt = m_groupOwnedMeshPageKeys.find(nextOwner);
                    if (nextPagesIt != m_groupOwnedPages.end() && nextKeysIt != m_groupOwnedMeshPageKeys.end()) {
                        const uint32_t count = std::min(
                            static_cast<uint32_t>(nextPagesIt->second.size()),
                            static_cast<uint32_t>(nextKeysIt->second.size()));
                        for (uint32_t nextSeg = 0; nextSeg < count; ++nextSeg) {
                            if (nextPagesIt->second[nextSeg] == page && nextKeysIt->second[nextSeg] == key) {
                                m_pageOwnerSegment[page] = nextSeg;
                                break;
                            }
                        }
                    }
                }
            }
            continue;
        }

        if (hadPendingReference &&
            (GetPendingMeshPageRefCount(page, key) != 0u ||
                (page < m_pendingPageOwnerGroup.size() &&
                    m_pendingPageOwnerGroup[page] != ~0u &&
                    m_pendingPageOwnerGroup[page] != groupIndex))) {
            continue;
        }

        RetirePhysicalPage(page, meshManager, IsPhysicalPagePinnedStorage(page));
    }

    m_groupOwnedPages.erase(pagesIt);
    m_groupOwnedMeshPageKeys.erase(groupIndex);
    SetGroupUsesPinnedStorage(groupIndex, false);

    if (meshManager != nullptr) {
        meshManager->EvictCLodGroupResidency(groupIndex, clearPageMapEntries);
    }
}

void CLodStreamingSystem::BeginPageProtectionUpdate() {
    for (uint32_t page : m_pagesProtectedThisUpdate) {
        if (page < m_pageProtectedThisUpdate.size()) {
            m_pageProtectedThisUpdate[page] = 0u;
        }
    }
    m_pagesProtectedThisUpdate.clear();
    for (uint32_t word : m_protectedGroupWordsScratch) {
        if (word < m_protectedGroupsBitsScratch.size()) {
            m_protectedGroupsBitsScratch[word] = 0u;
        }
    }
    m_protectedGroupWordsScratch.clear();
}

bool CLodStreamingSystem::MarkGroupProtectedThisUpdate(uint32_t groupIndex) {
    const uint32_t word = BitWordAddress(groupIndex);
    if (word >= m_protectedGroupsBitsScratch.size()) {
        m_protectedGroupsBitsScratch.resize(word + 1u, 0u);
    }

    const uint32_t mask = BitMask(groupIndex);
    uint32_t& bits = m_protectedGroupsBitsScratch[word];
    if ((bits & mask) != 0u) {
        return false;
    }

    if (bits == 0u) {
        m_protectedGroupWordsScratch.push_back(word);
    }
    bits |= mask;
    return true;
}

void CLodStreamingSystem::MarkPageProtectedThisUpdate(uint32_t page) {
    if (page >= m_pageProtectedThisUpdate.size()) {
        return;
    }
    if (m_pageProtectedThisUpdate[page] != 0u) {
        return;
    }

    m_pageProtectedThisUpdate[page] = 1u;
    m_pagesProtectedThisUpdate.push_back(page);
}

bool CLodStreamingSystem::TryGetCachedParentGroup(uint32_t groupIndex, uint32_t& outParentGroupIndex) {
    static constexpr uint32_t kUnknownParent = UINT32_MAX;
    static constexpr uint32_t kNoParent = UINT32_MAX - 1u;

    if (groupIndex >= m_parentGroupByGroup.size()) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    if (groupIndex >= m_parentGroupByGroup.size()) {
        return false;
    }

    uint32_t cachedParent = m_parentGroupByGroup[groupIndex];
    if (cachedParent == kUnknownParent) {
        cachedParent = kNoParent;
        if (ICLodGeometryStorage* meshManager = m_geometryStorage) {
            uint32_t parent = 0u;
            if (meshManager->TryGetCLodParentGroup(groupIndex, parent) && parent != groupIndex) {
                cachedParent = parent;
            }
        }
        m_parentGroupByGroup[groupIndex] = cachedParent;
    }

    if (cachedParent == kNoParent || cachedParent == kUnknownParent) {
        return false;
    }

    outParentGroupIndex = cachedParent;
    return true;
}

void CLodStreamingSystem::ProtectGroupAndAncestors(uint32_t groupIndex) {
    auto protectOne = [this](uint32_t g) -> bool {
        if (!MarkGroupProtectedThisUpdate(g)) {
            return false;
        }

        auto pagesIt = m_groupOwnedPages.find(g);
        if (pagesIt == m_groupOwnedPages.end()) {
            return true;
        }
        for (uint32_t page : pagesIt->second) {
            if (page != ~0u && page < m_pageProtectedThisUpdate.size()) {
                MarkPageProtectedThisUpdate(page);
                PageLruForPage(page).Touch(page);
            }
        }
        return true;
    };

    // Every group is protected through this function, so an already-protected
    // node implies that its selected ancestor chain was handled earlier.
    if (!protectOne(groupIndex)) {
        return;
    }
    uint32_t current = groupIndex;
    for (size_t hop = 0; hop < m_streamingStorageGroupCapacity; ++hop) {
        uint32_t parent = 0;
        if (!TryGetCachedParentGroup(current, parent) || parent == current) {
            break;
        }
        if (!protectOne(parent)) {
            break;
        }
        current = parent;
    }
}

bool CLodStreamingSystem::IsPhysicalPageCleanForFreshAllocation(uint32_t page) const {
    if (page >= m_pageState.size()) {
        return false;
    }
    if (m_pageState[page] != CLodPhysicalPageState::Free) {
        return false;
    }
    if (page < m_pageResidentGroups.size() && !m_pageResidentGroups[page].empty()) {
        return false;
    }
    if (page < m_pageOwnerMeshPageKey.size() && m_pageOwnerMeshPageKey[page] != kInvalidCLodMeshPageKey) {
        return false;
    }
    if (page < m_pageOwnerGroup.size() && m_pageOwnerGroup[page] >= 0) {
        return false;
    }
    if (page < m_pendingPageOwnerGroup.size() && m_pendingPageOwnerGroup[page] != ~0u) {
        return false;
    }
    return true;
}

bool CLodStreamingSystem::IsPhysicalPageEvictable(uint32_t page) const {
    if (page >= m_pageState.size()) {
        return false;
    }
    if (page < m_pageProtectedThisUpdate.size() && m_pageProtectedThisUpdate[page] != 0u) {
        return false;
    }
    if (m_pageState[page] != CLodPhysicalPageState::Resident) {
        return false;
    }
    return true;
}

bool CLodStreamingSystem::EvictPhysicalPage(uint32_t page, ICLodGeometryStorage* meshManager) {
    if (!IsPhysicalPageEvictable(page)) {
        return false;
    }

    std::vector<uint32_t> groupsToEvict;
    if (page < m_pageResidentGroups.size()) {
        groupsToEvict.assign(m_pageResidentGroups[page].begin(), m_pageResidentGroups[page].end());
    } else if (page < m_pageOwnerGroup.size() && m_pageOwnerGroup[page] >= 0) {
        groupsToEvict.push_back(static_cast<uint32_t>(m_pageOwnerGroup[page]));
    }

    for (uint32_t groupIndex : groupsToEvict) {
        // Residency must remain ancestor-closed. If this group disappears,
        // none of the finer groups reachable through its refinement edges may
        // remain visible to traversal.
        ForceGroupAndDescendantsNonResident(groupIndex, meshManager, false);
    }

    if (page < m_pageState.size() && m_pageState[page] != CLodPhysicalPageState::Retiring) {
        RetirePhysicalPage(page, meshManager, IsPhysicalPagePinnedStorage(page));
    }
    return true;
}

std::vector<uint32_t> CLodStreamingSystem::PopFreePages(
    std::span<const uint32_t> pageSizeBytes,
    ICLodGeometryStorage* meshManager,
    PagePopFailureStats* outStats) {
    ZoneScopedN("CLodStreamingSystem::PopFreePages");
    const uint32_t count = static_cast<uint32_t>(pageSizeBytes.size());
    ZoneValue(count);

    std::vector<uint32_t> pages;
    pages.reserve(count);
    PagePool* pool = meshManager != nullptr ? meshManager->GetCLodPagePool() : nullptr;
    if (count == 0u || pool == nullptr) {
        return pages;
    }
    const uint32_t desiredPageSize = pool->SelectPageSize(pageSizeBytes.front());
    CLodPageLRU& pageLru =
        m_pageLrus[pool->SelectPageSizeClassIndex(desiredPageSize)];

    const auto recordDirtyMetadata = [&]() {
        if (outStats != nullptr) {
            ++outStats->rejectedDirtyMetadata;
        }
    };
    const auto recordPendingWrite = [&]() {
        if (outStats != nullptr) {
            ++outStats->rejectedPendingWrite;
        }
    };
    const auto recordProtected = [&]() {
        if (outStats != nullptr) {
            ++outStats->rejectedProtected;
        }
    };

    const auto tryAcquireCleanFreePage = [&](uint32_t page) -> bool {
        if (page >= m_pageState.size()) {
            recordDirtyMetadata();
            return false;
        }
        if (m_pageState[page] == CLodPhysicalPageState::PreAllocatedCpuUpload ||
            m_pageState[page] == CLodPhysicalPageState::PendingDirectStorageWrite ||
            m_pageState[page] == CLodPhysicalPageState::Retiring) {
            recordPendingWrite();
            return false;
        }
        if (m_pageState[page] != CLodPhysicalPageState::Free || page >= m_pageOwnerGroup.size()) {
            return false;
        }

        m_pageOwnerGroup[page] = -1;
        m_pageOwnerSegment[page] = 0u;
        if (page < m_pageOwnerMeshPageKey.size()) {
            m_pageOwnerMeshPageKey[page] = kInvalidCLodMeshPageKey;
        }
        m_pendingPageOwnerGroup[page] = ~0u;
        m_pendingPageOwnerSegment[page] = 0u;
        if (outStats != nullptr) {
            ++outStats->freeClean;
        }

        PageLruForPage(page).Remove(page);
        if (!IsPhysicalPageCleanForFreshAllocation(page)) {
            recordDirtyMetadata();
            const uint64_t ownerKey = page < m_pageOwnerMeshPageKey.size()
                ? m_pageOwnerMeshPageKey[page]
                : kInvalidCLodMeshPageKey;
            spdlog::warn(
                "CLod streaming: refusing to allocate physical page {} because stale ownership remained after free cleanup (state={}, ownerGroup={}, ownerKey={}, residentGroups={}, pendingOwner={}, writeToken={})",
                page,
                page < m_pageState.size() ? static_cast<uint32_t>(m_pageState[page]) : UINT32_MAX,
                page < m_pageOwnerGroup.size() ? m_pageOwnerGroup[page] : -1,
                ownerKey,
                page < m_pageResidentGroups.size() ? static_cast<uint32_t>(m_pageResidentGroups[page].size()) : 0u,
                page < m_pendingPageOwnerGroup.size() ? m_pendingPageOwnerGroup[page] : UINT32_MAX,
                0u);
            return false;
        }

        pages.push_back(page);
        return true;
    };

    uint32_t lruSize = pageLru.Size();
    const uint32_t requestScanLimit = count > UINT32_MAX / 64u ? UINT32_MAX : count * 64u;
    const uint32_t backlogPressure = std::max<uint32_t>(m_pendingStreamingRequestCount, m_streamingRequestsInProgressCount);
    const uint32_t pressureScanLimit = std::clamp<uint32_t>(backlogPressure / 8u, 64u, 2048u);
    const uint32_t desiredScanLimit = std::max<uint32_t>(
        256u,
        std::max<uint32_t>(requestScanLimit, pressureScanLimit));
    const uint32_t scanLimit = std::min<uint32_t>(lruSize, desiredScanLimit);
    if (outStats != nullptr) {
        outStats->scanLimit = scanLimit;
        outStats->evictionBudgetLimit = m_pagePopEvictionBudgetThisUpdate;
        outStats->evictionsUsed = m_pagePopEvictionsThisUpdate;
    }
    const auto stampFinalStats = [&]() {
        if (outStats != nullptr) {
            outStats->evictionsUsed = m_pagePopEvictionsThisUpdate;
        }
    };

    {
        ZoneScopedN("CLodStreamingSystem::PopFreePages::ScanFreePages");
        uint32_t attemptsRemaining = scanLimit;
        while (pages.size() < count && attemptsRemaining-- > 0u) {
            uint32_t page = pageLru.PopOldest();
            if (page == ~0u) {
                break;
            }
            if (outStats != nullptr) {
                ++outStats->scanned;
            }

            if (page < m_pageProtectedThisUpdate.size() && m_pageProtectedThisUpdate[page] != 0u) {
                recordProtected();
                break;
            }

            if (tryAcquireCleanFreePage(page)) {
                continue;
            }
        }
    }

    if (pages.size() >= count) {
        stampFinalStats();
        return pages;
    }

    {
        std::vector<uint32_t> newPages =
            pool->GrowGeneralPageClass(desiredPageSize);
        if (!newPages.empty()) {
            EnsurePageTrackingCapacity(meshManager);
            for (uint32_t page : newPages) {
                pageLru.Insert(page);
            }
            while (pages.size() < count) {
                const uint32_t page = pageLru.PopOldest();
                if (page == ~0u || !tryAcquireCleanFreePage(page)) {
                    break;
                }
            }
            if (pages.size() >= count) {
                stampFinalStats();
                return pages;
            }
            lruSize = pageLru.Size();
        }
    }

    {
        ZoneScopedN("CLodStreamingSystem::PopFreePages::EvictResidentPages");
        uint32_t attemptsRemaining = scanLimit;
        while (pages.size() < count && attemptsRemaining-- > 0u) {
            uint32_t page = pageLru.PopOldest();
            if (page == ~0u) break;
            if (outStats != nullptr) {
                ++outStats->scanned;
            }

            if (page < m_pageProtectedThisUpdate.size() && m_pageProtectedThisUpdate[page] != 0u) {
                recordProtected();
                break;
            }
            if (page >= m_pageState.size()) {
                recordDirtyMetadata();
                continue;
            }
            if (m_pageState[page] == CLodPhysicalPageState::PreAllocatedCpuUpload ||
                m_pageState[page] == CLodPhysicalPageState::PendingDirectStorageWrite ||
                m_pageState[page] == CLodPhysicalPageState::Retiring) {
                recordPendingWrite();
                continue;
            }
            if (page < m_pageResidentGroups.size() && !m_pageResidentGroups[page].empty()) {
                ScrubStaleResidentGroups(page);
            }

            if (page < m_pageResidentGroups.size() && !m_pageResidentGroups[page].empty()) {
                if (m_pagePopEvictionsThisUpdate >= m_pagePopEvictionBudgetThisUpdate) {
                    if (outStats != nullptr) {
                        ++outStats->rejectedEvictionBudget;
                    }
                    break;
                }
                if (!EvictPhysicalPage(page, meshManager)) {
                    if (outStats != nullptr) {
                        ++outStats->rejectedEvictFailed;
                    }
                    continue;
                }
                if (outStats != nullptr) {
                    ++outStats->evicted;
                }
                ++m_pagePopEvictionsThisUpdate;
                continue;
            } else if (m_pageState[page] == CLodPhysicalPageState::Resident &&
                page < m_pageOwnerGroup.size() &&
                m_pageOwnerGroup[page] >= 0) {
                if (m_pagePopEvictionsThisUpdate >= m_pagePopEvictionBudgetThisUpdate) {
                    if (outStats != nullptr) {
                        ++outStats->rejectedEvictionBudget;
                    }
                    break;
                }
                if (!EvictPhysicalPage(page, meshManager)) {
                    if (outStats != nullptr) {
                        ++outStats->rejectedEvictFailed;
                    }
                    continue;
                }
                if (outStats != nullptr) {
                    ++outStats->evicted;
                }
                ++m_pagePopEvictionsThisUpdate;
                continue;
            } else if (tryAcquireCleanFreePage(page)) {
                continue;
            } else {
                recordDirtyMetadata();
                continue;
            }
        }
    }

    stampFinalStats();
    return pages;
}

CLodStreamingSystem::PreAllocatedPages CLodStreamingSystem::PreAllocatePagesForGroup(
    uint32_t groupIndex, const br::render::CLodGroupStreamingInfo& info, ICLodGeometryStorage* meshManager) {
    return PreAllocatePagesForGroup(
        groupIndex,
        info.groupsBase,
        std::span<const uint32_t>(
            info.meshPageIndices.data(),
            info.meshPageIndices.size()),
        std::span<const uint32_t>(
            info.meshPageBlobSizes.data(),
            info.meshPageBlobSizes.size()),
        meshManager,
        info.valid);
}

CLodStreamingSystem::PreAllocatedPages CLodStreamingSystem::PreAllocatePagesForGroup(
    uint32_t groupIndex,
    uint32_t groupsBase,
    std::span<const uint32_t> meshPageIndices,
    std::span<const uint32_t> meshPageBlobSizes,
    ICLodGeometryStorage* meshManager,
    bool buildMeshPageKeys) {
    ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup");

    const uint32_t segmentCount = buildMeshPageKeys
        ? static_cast<uint32_t>(meshPageIndices.size())
        : 1u;
    ZoneValue(segmentCount);
    PreAllocatedPages result;
    {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::InitializeResult");
        result.segmentCount = segmentCount;
        result.pagesBySegment.assign(segmentCount, ~0u);
        result.segmentNeedsFetch.assign(segmentCount, true);
        result.meshPageKeys.assign(segmentCount, kInvalidCLodMeshPageKey);
        result.usesPinnedStorage = IsGroupPinned(groupIndex);
    }

    {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::EnsurePageTrackingCapacity");
        EnsurePageTrackingCapacity(meshManager);
    }

    if (buildMeshPageKeys && meshPageIndices.size() == segmentCount) {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::BuildMeshPageKeys");
        for (uint32_t seg = 0; seg < segmentCount; ++seg) {
            result.meshPageKeys[seg] =
                MakeCLodMeshPageKey(groupsBase, meshPageIndices[seg]);
        }
    }

    uint32_t missingCount = 0;
    {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::LookupExistingPages");
        for (uint32_t seg = 0; seg < segmentCount; ++seg) {
            const uint64_t meshPageKey = result.meshPageKeys[seg];
            if (meshPageKey == kInvalidCLodMeshPageKey) {
                ++missingCount;
                continue;
            }

            auto residentIt = m_residentMeshPageToPhysicalPage.find(meshPageKey);
            if (residentIt != m_residentMeshPageToPhysicalPage.end() &&
                IsPhysicalPageResidentForKey(residentIt->second, meshPageKey)) {
                const uint32_t existingPage = residentIt->second;
                result.pagesBySegment[seg] = existingPage;
                result.segmentNeedsFetch[seg] = false;
                MarkPageProtectedThisUpdate(existingPage);
                if (!IsPhysicalPagePinnedStorage(existingPage)) {
                    PageLruForPage(existingPage).Touch(existingPage);
                }
                continue;
            }

            auto pendingIt = m_pendingMeshPageToPhysicalPage.find(meshPageKey);
            if (pendingIt != m_pendingMeshPageToPhysicalPage.end() &&
                IsPhysicalPagePendingForKey(pendingIt->second, meshPageKey)) {
                const uint32_t existingPage = pendingIt->second;
                result.pagesBySegment[seg] = existingPage;
                result.segmentNeedsFetch[seg] = false;
                MarkPageProtectedThisUpdate(existingPage);
                if (!IsPhysicalPagePinnedStorage(existingPage)) {
                    PageLruForPage(existingPage).Touch(existingPage);
                }
                spdlog::debug(
                    "CLod streaming: group {} reusing pending physical page {} for mesh-page key {} seg {}",
                    groupIndex,
                    existingPage,
                    meshPageKey,
                    seg);
                continue;
            }

            ++missingCount;
        }
    }

    std::vector<uint32_t> freshPages;
    std::vector<uint32_t> missingPageSizes;
    missingPageSizes.reserve(missingCount);
    PagePool* pool = meshManager != nullptr ? meshManager->GetCLodPagePool() : nullptr;
    for (uint32_t seg = 0u; seg < segmentCount; ++seg) {
        if (result.pagesBySegment[seg] == ~0u) {
            missingPageSizes.push_back(
                seg < meshPageBlobSizes.size() && meshPageBlobSizes[seg] != 0u
                    ? meshPageBlobSizes[seg]
                    : (pool != nullptr ? static_cast<uint32_t>(pool->GetPageSize()) : 256u * 1024u));
        }
    }
    if (missingCount == 0u) {
        freshPages.clear();
    } else if (result.usesPinnedStorage) {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::AllocatePinnedPages");
        ZoneValue(missingCount);
        if (pool == nullptr) {
            return PreAllocatedPages{};
        }

        freshPages = pool->AllocatePinnedPages(missingPageSizes);
        {
            ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::AllocatePinnedPages::EnsurePageTrackingCapacity");
            EnsurePageTrackingCapacity(meshManager);
        }
        if (freshPages.size() < missingCount) {
            pool->FreePinnedPages(freshPages);
            return PreAllocatedPages{};
        }
        for (uint32_t page : freshPages) {
            if (page < m_pagePinnedStorage.size()) {
                m_pagePinnedStorage[page] = 1u;
            }
        }
    } else {
        // Pop fresh pages for missing segments.
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::PopFreePages");
        ZoneValue(missingCount);
        PagePopFailureStats popStats{};
        for (uint32_t requestedSize : missingPageSizes) {
            const std::array<uint32_t, 1> singleRequest{ requestedSize };
            std::vector<uint32_t> allocated =
                PopFreePages(singleRequest, meshManager, &popStats);
            if (allocated.empty()) {
                break;
            }
            freshPages.push_back(allocated.front());
        }
        if (freshPages.size() < missingCount) {
            {
                ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::PopFreePages::FailureDiagnostics");
                static uint64_t s_lastPreallocationFailureLogTick = 0;
                if (m_streamingDiagnosticTick >= s_lastPreallocationFailureLogTick + 120u) {
                    s_lastPreallocationFailureLogTick = m_streamingDiagnosticTick;
                    uint32_t livePreallocatedFreshPages = 0u;
                    for (const auto& [_, pages] : m_preAllocatedPagesByGroup) {
                        for (uint32_t seg = 0; seg < pages.segmentCount; ++seg) {
                            if (seg < pages.segmentNeedsFetch.size() && pages.segmentNeedsFetch[seg]) {
                                ++livePreallocatedFreshPages;
                            }
                        }
                    }
                    spdlog::debug(
                        "CLod streaming diag[tick={}]: preallocation failed for group {} missingPages={} acquired={} lruSize={} scanned={} scanLimit={} reject(protected={}, pendingWrite={}, evictFailed={}, evictionBudget={}, dirtyMetadata={}) evicted={} evictionBudgetLimit={} evictionsUsed={} freeClean={} pendingCpu={} inProgress={} residentGroups={} preallocGroups={} preallocFreshPages={}",
                        m_streamingDiagnosticTick,
                        groupIndex,
                        missingCount,
                        static_cast<uint32_t>(freshPages.size()),
                        TotalPageLruSize(),
                        popStats.scanned,
                        popStats.scanLimit,
                        popStats.rejectedProtected,
                        popStats.rejectedPendingWrite,
                        popStats.rejectedEvictFailed,
                        popStats.rejectedEvictionBudget,
                        popStats.rejectedDirtyMetadata,
                        popStats.evicted,
                        popStats.evictionBudgetLimit,
                        popStats.evictionsUsed,
                        popStats.freeClean,
                        m_pendingStreamingRequestCount,
                        m_streamingRequestsInProgressCount,
                        m_streamingResidentGroupsCount,
                        static_cast<uint32_t>(m_preAllocatedPagesByGroup.size()),
                        livePreallocatedFreshPages);
                }
            }
            {
                ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::PopFreePages::RestorePartialPages");
                for (uint32_t page : freshPages) {
                    if (IsPhysicalPageCleanForFreshAllocation(page)) {
                        PageLruForPage(page).Insert(page);
                    }
                }
            }
            ReleasePreAllocatedPages(result, meshManager);
            return PreAllocatedPages{}; // empty = failure
        }
        for (uint32_t page : freshPages) {
            if (page < m_pagePinnedStorage.size()) {
                m_pagePinnedStorage[page] = 0u;
            }
        }
    }

    // Assign fresh pages to missing segments.
    {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::AssignFreshPages");
        uint32_t freshIdx = 0;
        for (uint32_t seg = 0; seg < segmentCount; ++seg) {
            if (result.pagesBySegment[seg] == ~0u) {
                result.pagesBySegment[seg] = freshPages[freshIdx++];
                result.segmentNeedsFetch[seg] = true;
            }
        }
    }

    {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::MarkPreAllocatedPages");
        for (uint32_t seg = 0; seg < segmentCount; ++seg) {
            const uint32_t page = result.pagesBySegment[seg];
            if (page == ~0u || page >= m_pageState.size()) {
                continue;
            }
            if (result.segmentNeedsFetch[seg]) {
                // Keep in-flight upload targets out of the LRU so another request
                // cannot reuse the physical page before this IO completes/cancels.
                m_pageOwnerGroup[page] = static_cast<int32_t>(groupIndex);
                m_pageOwnerSegment[page] = seg;
                m_pageState[page] = CLodPhysicalPageState::PreAllocatedCpuUpload;
                m_pendingPageOwnerGroup[page] = groupIndex;
                m_pendingPageOwnerSegment[page] = seg;
                if (page < m_pageOwnerMeshPageKey.size()) {
                    m_pageOwnerMeshPageKey[page] = result.meshPageKeys[seg];
                }
                if (result.meshPageKeys[seg] != kInvalidCLodMeshPageKey) {
                    m_pendingMeshPageToPhysicalPage[result.meshPageKeys[seg]] = page;
                }
            }
        }
    }

    return result;
}

bool CLodStreamingSystem::AssignPagesToGroup(uint32_t groupIndex, const PreAllocatedPages& pages, ICLodGeometryStorage* meshManager) {
    for (uint32_t seg = 0; seg < pages.segmentCount; ++seg) {
        const uint32_t page = seg < pages.pagesBySegment.size() ? pages.pagesBySegment[seg] : ~0u;
        const uint64_t meshPageKey = seg < pages.meshPageKeys.size() ? pages.meshPageKeys[seg] : kInvalidCLodMeshPageKey;
        ScrubStaleResidentGroups(page);
        if (page == ~0u ||
            page >= m_pageOwnerGroup.size() ||
            page >= m_pageState.size() ||
            page >= m_pageOwnerMeshPageKey.size() ||
            meshPageKey == kInvalidCLodMeshPageKey) {
            spdlog::warn(
                "CLod streaming: refusing page assignment for group {} seg {} page {} key {} because the page/key is invalid",
                groupIndex,
                seg,
                page,
                meshPageKey);
            return false;
        }

        const bool fetchedPage =
            seg < pages.segmentNeedsFetch.size() &&
            pages.segmentNeedsFetch[seg];

        if (m_pageOwnerMeshPageKey[page] != meshPageKey) {
            spdlog::warn(
                "CLod streaming: refusing page assignment for group {} seg {} page {} key {} because physical page is owned by key {}",
                groupIndex,
                seg,
                page,
                meshPageKey,
                m_pageOwnerMeshPageKey[page]);
            return false;
        }

        if (fetchedPage) {
            const auto residentIt = m_residentMeshPageToPhysicalPage.find(meshPageKey);
            if (residentIt != m_residentMeshPageToPhysicalPage.end() &&
                residentIt->second != page &&
                IsPhysicalPageResidentForKey(residentIt->second, meshPageKey)) {
                spdlog::warn(
                    "CLod streaming: refusing fetched page assignment for group {} seg {} page {} key {} because the key is already resident on page {}",
                    groupIndex,
                    seg,
                    page,
                    meshPageKey,
                    residentIt->second);
                return false;
            }
            if (m_pendingPageOwnerGroup[page] != groupIndex) {
                spdlog::warn(
                    "CLod streaming: refusing fetched page assignment for group {} seg {} page {} key {} because pending ownership changed",
                    groupIndex,
                    seg,
                    page,
                    meshPageKey);
                return false;
            }
        } else if (!IsPhysicalPageResidentForKey(page, meshPageKey) &&
            !IsPhysicalPagePendingForKey(page, meshPageKey)) {
                spdlog::warn(
                    "CLod streaming: refusing resident page assignment for group {} seg {} page {} key {} because resident mapping changed",
                    groupIndex,
                    seg,
                    page,
                    meshPageKey);
                return false;
        }
    }

    if (m_groupOwnedPages.find(groupIndex) != m_groupOwnedPages.end()) {
        ReleaseGroupResidency(groupIndex, meshManager, true);
    }

    m_groupOwnedPages[groupIndex] = pages.pagesBySegment;
    m_groupOwnedMeshPageKeys[groupIndex] = pages.meshPageKeys;
    SetGroupUsesPinnedStorage(groupIndex, pages.usesPinnedStorage);

    for (uint32_t seg = 0; seg < pages.segmentCount; ++seg) {
        uint32_t page = pages.pagesBySegment[seg];
        if (page != ~0u && page < m_pageOwnerGroup.size()) {
            const uint64_t meshPageKey = seg < pages.meshPageKeys.size() ? pages.meshPageKeys[seg] : kInvalidCLodMeshPageKey;
            const bool fetchedPage =
                seg < pages.segmentNeedsFetch.size() &&
                pages.segmentNeedsFetch[seg];
            if (fetchedPage && page < m_pageOwnerMeshPageKey.size()) {
                m_pageOwnerMeshPageKey[page] = meshPageKey;
            }
            if (fetchedPage) {
                m_pageState[page] = CLodPhysicalPageState::PreAllocatedCpuUpload;
                m_pendingPageOwnerGroup[page] = groupIndex;
                m_pendingPageOwnerSegment[page] = seg;
                AddPendingMeshPageReference(page, meshPageKey);
                PageLruForPage(page).Remove(page);
            } else if (IsPhysicalPageResidentForKey(page, meshPageKey)) {
                m_pageState[page] = CLodPhysicalPageState::Resident;
                m_pendingPageOwnerGroup[page] = ~0u;
                m_pendingPageOwnerSegment[page] = 0u;
                m_residentMeshPageToPhysicalPage[meshPageKey] = page;
                m_residentMeshPageRefCounts[meshPageKey]++;
                if (page < m_pageResidentGroups.size()) {
                    m_pageResidentGroups[page].insert(groupIndex);
                }
                m_pageOwnerGroup[page] = static_cast<int32_t>(groupIndex);
                m_pageOwnerSegment[page] = seg;
            } else {
                // Another group is already uploading this mesh page into this
                // physical slot. This group can reference the same slot, but
                // it must not publish resident state until the shared upload
                // drains.
                AddPendingMeshPageReference(page, meshPageKey);
                PageLruForPage(page).Remove(page);
            }
            if (!IsPhysicalPagePinnedStorage(page) && !fetchedPage) {
                if (IsPhysicalPageResidentForKey(page, meshPageKey)) {
                    PageLruForPage(page).Insert(page);
                }
            }
        }
    }
    return true;
}

void CLodStreamingSystem::ReleasePreAllocatedPages(const PreAllocatedPages& pages, ICLodGeometryStorage* meshManager) {
    for (uint32_t seg = 0; seg < pages.segmentCount; ++seg) {
        uint32_t page = pages.pagesBySegment[seg];
        if (page == ~0u) continue;

        const bool fetchedPage =
            seg < pages.segmentNeedsFetch.size() &&
            pages.segmentNeedsFetch[seg];
        if (pages.usesPinnedStorage) {
            const uint64_t meshPageKey = seg < pages.meshPageKeys.size() ? pages.meshPageKeys[seg] : kInvalidCLodMeshPageKey;
            if (IsPhysicalPageResidentForKey(page, meshPageKey)) {
                continue;
            }

            if (fetchedPage) {
                // A completion may outlive its preallocation. Never retire a
                // physical page that has since been released or reassigned.
                if (page >= m_pageOwnerMeshPageKey.size() ||
                    m_pageOwnerMeshPageKey[page] != meshPageKey) {
                    continue;
                }
                RetirePhysicalPage(page, meshManager, IsPhysicalPagePinnedStorage(page));
            } else if (meshManager != nullptr) {
                if (IsPhysicalPagePinnedStorage(page)) {
                    if (PagePool* pool = meshManager->GetCLodPagePool()) {
                        pool->FreePinnedPages(std::vector<uint32_t>{ page });
                    }
                    if (page < m_pagePinnedStorage.size()) {
                        m_pagePinnedStorage[page] = 0u;
                    }
                } else if (IsPhysicalPageCleanForFreshAllocation(page)) {
                    PageLruForPage(page).Insert(page);
                }
            }
            continue;
        }

        if (fetchedPage) {
            const uint64_t meshPageKey = seg < pages.meshPageKeys.size() ? pages.meshPageKeys[seg] : kInvalidCLodMeshPageKey;
            if (IsPhysicalPageResidentForKey(page, meshPageKey)) {
                continue;
            }

            // Stale completion cleanup must not retire the page's new owner.
            if (page >= m_pageOwnerMeshPageKey.size() ||
                m_pageOwnerMeshPageKey[page] != meshPageKey) {
                continue;
            }
            RetirePhysicalPage(page, meshManager, IsPhysicalPagePinnedStorage(page));
        }
    }
}

bool CLodStreamingSystem::ValidateRenderableCompletion(
    uint32_t groupIndex,
    const PreAllocatedPages& pages,
    const br::render::CLodDiskStreamingCompletion& completion,
    uint32_t expectedPageCount,
    ICLodGeometryStorage* meshManager) const {
    if (expectedPageCount == 0u) {
        return completion.meshPageIndices.empty() &&
            completion.preAllocatedPages.empty() &&
            completion.pageAllocations.empty() &&
            completion.pageMapEntries.empty();
    }

    const bool fetchMaskValid = completion.segmentNeedsFetch.empty() ||
        completion.segmentNeedsFetch.size() == expectedPageCount;
    const bool preAllocFetchMaskValid = pages.segmentNeedsFetch.size() == expectedPageCount;
    if (!fetchMaskValid ||
        !preAllocFetchMaskValid ||
        pages.segmentCount != expectedPageCount ||
        pages.pagesBySegment.size() != expectedPageCount ||
        pages.meshPageKeys.size() != expectedPageCount ||
        completion.meshPageIndices.size() != expectedPageCount ||
        completion.preAllocatedPages.size() != expectedPageCount ||
        completion.pageAllocations.size() != expectedPageCount ||
        completion.pageMapEntries.size() != expectedPageCount) {
        spdlog::warn(
            "CLod streaming: rejecting completion for group {} because it does not cover all {} required pages",
            groupIndex,
            expectedPageCount);
        return false;
    }

    for (uint32_t seg = 0; seg < expectedPageCount; ++seg) {
        const uint32_t page = pages.pagesBySegment[seg];
        if (page == ~0u || page >= m_pageState.size()) {
            spdlog::warn(
                "CLod streaming: rejecting completion for group {} because segment {} has no valid physical page",
                groupIndex,
                seg);
            return false;
        }

        if (pages.meshPageKeys[seg] == kInvalidCLodMeshPageKey) {
            spdlog::warn(
                "CLod streaming: rejecting completion for group {} because segment {} has no mesh-page key",
                groupIndex,
                seg);
            return false;
        }

        if (completion.preAllocatedPages[seg] != page ||
            completion.pageAllocations[seg].firstPageID != page ||
            !completion.pageAllocations[seg].IsValid()) {
            spdlog::warn(
                "CLod streaming: rejecting completion for group {} because segment {} page allocation does not match the residency preallocation",
                groupIndex,
                seg);
            return false;
        }

        if (completion.pageMapEntries[seg].slabDescriptorIndex == 0u) {
            spdlog::warn(
                "CLod streaming: rejecting completion for group {} because segment {} has a zero slab descriptor (page={}, key={})",
                groupIndex,
                seg,
                page,
                pages.meshPageKeys[seg]);
            return false;
        }

        // Validate cache/source ownership before upload as a second, independent
        // observation point. This had been disabled after an earlier page-lifecycle
        // fix, which allowed a persistent wrong-source payload to reach the GPU with
        // only mesh-local group IDs available for diagnosis.
        const auto info = meshManager
            ? meshManager->GetCLodGroupStreamingInfo(groupIndex)
            : br::render::CLodGroupStreamingInfo{};
        const bool needsFetch = completion.segmentNeedsFetch.empty() ||
            seg >= static_cast<uint32_t>(completion.segmentNeedsFetch.size()) ||
            completion.segmentNeedsFetch[seg];
        const uint32_t expectedLocalGroup = info.valid && groupIndex >= info.groupsBase
            ? groupIndex - info.groupsBase
            : UINT32_MAX;
        if (needsFetch &&
            expectedLocalGroup != UINT32_MAX &&
            seg < static_cast<uint32_t>(completion.pageBlobs.size())) {
            const auto pageBlob = std::span<const std::byte>(
                completion.pageBlobs[seg].data(),
                completion.pageBlobs[seg].size());
            const uint32_t meshPageIndex = seg < static_cast<uint32_t>(completion.meshPageIndices.size())
                ? completion.meshPageIndices[seg]
                : UINT32_MAX;
            if (!CLodTrianglePageHasSourceGroup(pageBlob, expectedLocalGroup)) {
                spdlog::error(
                    "CLod streaming: fetched page payload for group {} localGroup={} seg={} meshPage={} page={} key={} slabMap={}:{} contains no clusters tagged with that local group",
                    groupIndex,
                    expectedLocalGroup,
                    seg,
                    meshPageIndex,
                    page,
                    pages.meshPageKeys[seg],
                    completion.pageMapEntries[seg].slabDescriptorIndex,
                    completion.pageMapEntries[seg].slabByteOffset);
            }
            ValidateCLodTrianglePageSegmentSourceGroups(
                pageBlob,
                expectedLocalGroup,
                meshPageIndex,
                info,
                groupIndex,
                seg,
                page,
                completion.pageMapEntries[seg]);
            ValidateCLodTrianglePageAllReferencedSegmentSourceGroups(
                pageBlob,
                meshPageIndex,
                info,
                groupIndex,
                seg,
                page,
                completion.pageMapEntries[seg]);
        }
    }

    return true;
}

