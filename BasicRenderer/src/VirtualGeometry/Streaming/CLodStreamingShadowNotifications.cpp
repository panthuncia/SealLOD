#include "VirtualGeometry/Streaming/CLodStreamingSystem.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <BasicTelemetry/Telemetry.h>
#include <tracy/Tracy.hpp>

#include "Runtime/Settings/SettingsManager.h"

void CLodStreamingSystem::QueueVirtualShadowReadyDependency(
    const VirtualShadowDependency& dependency) {
    const uint32_t physicalPageIndex = dependency.page.physicalPageIndex;
    if (physicalPageIndex >= CLodVirtualShadowMaxPhysicalPageCount) {
        ++m_virtualShadowUpgradeStats.staleEvents;
        return;
    }
    if (m_virtualShadowReadyByPhysicalPage.size() <
        CLodVirtualShadowMaxPhysicalPageCount) {
        m_virtualShadowReadyByPhysicalPage.resize(
            CLodVirtualShadowMaxPhysicalPageCount);
        m_virtualShadowReadyFlagsByPhysicalPage.resize(
            CLodVirtualShadowMaxPhysicalPageCount,
            0u);
    }
    if (m_virtualShadowReadyFlagsByPhysicalPage[physicalPageIndex] == 0u) {
        m_virtualShadowReadyFlagsByPhysicalPage[physicalPageIndex] = 1u;
        m_virtualShadowReadyTouchedPhysicalPages.push_back(physicalPageIndex);
    } else {
        ++m_virtualShadowUpgradeStats.dependenciesDeduplicated;
    }
    m_virtualShadowReadyByPhysicalPage[physicalPageIndex] = dependency;
}

void CLodStreamingSystem::RehashVirtualShadowDependencyBucket(
    VirtualShadowDependencyBucket& bucket,
    size_t capacity) {
    capacity = std::max<size_t>(8u, std::bit_ceil(capacity));
    const size_t previousCapacity = bucket.dependencies.size();
    auto previous = std::move(bucket.dependencies);
    bucket.dependencies.assign(capacity, VirtualShadowDependency{});
    m_virtualShadowActiveDependencySlotCount += capacity;
    m_virtualShadowActiveDependencySlotCount -= previousCapacity;
    basic_telemetry::AddCounter(
        "CLod.VSM.DependencyBucketRehashes");
    bucket.dependencyCount = 0u;
    const size_t mask = capacity - 1u;
    for (const auto& dependency : previous) {
        const uint32_t physicalPageIndex =
            dependency.page.physicalPageIndex;
        if (physicalPageIndex == UINT32_MAX) {
            continue;
        }
        size_t slot = static_cast<size_t>(physicalPageIndex) & mask;
        while (bucket.dependencies[slot].page.physicalPageIndex !=
               UINT32_MAX) {
            slot = (slot + 1u) & mask;
        }
        bucket.dependencies[slot] = dependency;
        ++bucket.dependencyCount;
    }
}

bool CLodStreamingSystem::InsertVirtualShadowDependency(
    VirtualShadowDependencyBucket& bucket,
    const VirtualShadowDependency& dependency) {
    if (bucket.dependencies.empty()) {
        RehashVirtualShadowDependencyBucket(bucket, 8u);
    }
    for (;;) {
        const size_t mask = bucket.dependencies.size() - 1u;
        const uint32_t physicalPageIndex =
            dependency.page.physicalPageIndex;
        size_t slot = static_cast<size_t>(physicalPageIndex) & mask;
        while (bucket.dependencies[slot].page.physicalPageIndex !=
                   UINT32_MAX &&
               bucket.dependencies[slot].page.physicalPageIndex !=
                   physicalPageIndex) {
            slot = (slot + 1u) & mask;
        }
        if (bucket.dependencies[slot].page.physicalPageIndex ==
            physicalPageIndex) {
            bucket.dependencies[slot] = dependency;
            return false;
        }
        if ((static_cast<size_t>(bucket.dependencyCount) + 1u) * 2u >
            bucket.dependencies.size()) {
            RehashVirtualShadowDependencyBucket(
                bucket,
                bucket.dependencies.size() * 2u);
            continue;
        }
        bucket.dependencies[slot] = dependency;
        ++bucket.dependencyCount;
        return true;
    }
}

CLodStreamingSystem::VirtualShadowDependencyBucket&
CLodStreamingSystem::GetOrCreateVirtualShadowDependencyBucket(
    uint32_t groupIndex) {
    if (groupIndex >=
        m_virtualShadowDependencyBucketIndexByGroup.size()) {
        m_virtualShadowDependencyBucketIndexByGroup.resize(
            static_cast<size_t>(groupIndex) + 1u,
            -1);
    }
    int32_t& bucketIndex =
        m_virtualShadowDependencyBucketIndexByGroup[groupIndex];
    if (bucketIndex < 0) {
        std::vector<VirtualShadowDependency> dependencies;
        if (!m_virtualShadowDependencyBucketPool.empty()) {
            dependencies = std::move(
                m_virtualShadowDependencyBucketPool.back());
            m_virtualShadowDependencyBucketPool.pop_back();
            std::fill(
                dependencies.begin(),
                dependencies.end(),
                VirtualShadowDependency{});
        }
        bucketIndex = static_cast<int32_t>(
            m_virtualShadowDependencyBuckets.size());
        m_virtualShadowDependencyBuckets.push_back(
            VirtualShadowDependencyBucket{
                groupIndex,
                0u,
                std::move(dependencies)});
        m_virtualShadowActiveDependencySlotCount +=
            m_virtualShadowDependencyBuckets.back()
                .dependencies.size();
    }
    return m_virtualShadowDependencyBuckets[
        static_cast<size_t>(bucketIndex)];
}

std::vector<CLodStreamingSystem::VirtualShadowDependency>
CLodStreamingSystem::RemoveVirtualShadowDependencyBucket(uint32_t groupIndex) {
    if (groupIndex >= m_virtualShadowDependencyBucketIndexByGroup.size()) {
        return {};
    }
    const int32_t bucketIndex =
        m_virtualShadowDependencyBucketIndexByGroup[groupIndex];
    if (bucketIndex < 0 ||
        static_cast<size_t>(bucketIndex) >=
            m_virtualShadowDependencyBuckets.size()) {
        return {};
    }

    auto& removedBucket =
        m_virtualShadowDependencyBuckets[static_cast<size_t>(bucketIndex)];
    auto dependencies = std::move(removedBucket.dependencies);
    m_virtualShadowActiveDependencyPairCount -= std::min<uint64_t>(
        m_virtualShadowActiveDependencyPairCount,
        removedBucket.dependencyCount);
    m_virtualShadowActiveDependencySlotCount -= std::min<uint64_t>(
        m_virtualShadowActiveDependencySlotCount,
        dependencies.size());
    const size_t lastIndex = m_virtualShadowDependencyBuckets.size() - 1u;
    if (static_cast<size_t>(bucketIndex) != lastIndex) {
        m_virtualShadowDependencyBuckets[static_cast<size_t>(bucketIndex)] =
            std::move(m_virtualShadowDependencyBuckets[lastIndex]);
        m_virtualShadowDependencyBucketIndexByGroup[
            m_virtualShadowDependencyBuckets[static_cast<size_t>(bucketIndex)]
                .groupIndex] = bucketIndex;
    }
    m_virtualShadowDependencyBuckets.pop_back();
    m_virtualShadowDependencyBucketIndexByGroup[groupIndex] = -1;
    return dependencies;
}

void CLodStreamingSystem::RecordVirtualShadowUpgradeDependencies(
    std::span<const CLodVirtualShadowPredictedPage> requests) {
    if (requests.empty()) {
        return;
    }
    if (!SettingsManager::GetInstance()
             .getSettingGetter<bool>(
                 CLodDirectionalVirtualShadowPredictiveLodInvalidationSettingName)()) {
        return;
    }
    ZoneScopedN("CLodStreamingSystem::RecordVirtualShadowUpgradeDependencies");
    BASIC_TELEMETRY_SCOPE("CLod.VSM.DependencyBatch");
    size_t expandedPairCount = 0u;

    ++m_virtualShadowBatchSourceGeneration;
    if (m_virtualShadowBatchSourceGeneration == 0u) {
        m_virtualShadowBatchSourceGeneration = 1u;
        std::fill(
            m_virtualShadowBatchSourceGenerationByGroup.begin(),
            m_virtualShadowBatchSourceGenerationByGroup.end(),
            0u);
    }
    m_virtualShadowMissingGroupsScratch.clear();

    for (uint32_t inputIndex = 0u;
         inputIndex < requests.size();
         ++inputIndex) {
        const auto& request = requests[inputIndex];
        const uint32_t sourceGroup = request.sourceGroupGlobalIndex;
        if (sourceGroup >= m_virtualShadowResidencyGenerationByGroup.size()) {
            m_virtualShadowResidencyGenerationByGroup.resize(
                static_cast<size_t>(sourceGroup) + 1u,
                1u);
        }
        if (IsGroupResident(sourceGroup)) {
            // Readback trails promotion. Queue the exact latest token directly
            // when the source group is already resident.
            QueueVirtualShadowReadyDependency(VirtualShadowDependency{
                CLodVirtualShadowPageToken{
                    request.PhysicalPageIndex(),
                    request.allocationGeneration,
                    request.contentGeneration,
                    request.ClipmapIndex(),
                    request.VirtualAddress()},
                m_virtualShadowResidencyGenerationByGroup[sourceGroup]});
            ++m_virtualShadowUpgradeStats.dependenciesObserved;
            ++m_virtualShadowUpgradeStats.lateResidentDependencies;
            ++m_virtualShadowUpgradeStats.eventsQueued;
            continue;
        }

        if (sourceGroup >=
            m_virtualShadowBatchSourceGenerationByGroup.size()) {
            const size_t newSize =
                static_cast<size_t>(sourceGroup) + 1u;
            m_virtualShadowBatchSourceGenerationByGroup.resize(
                newSize,
                0u);
            m_virtualShadowBatchSourceChainOffsetByGroup.resize(
                newSize,
                0u);
            m_virtualShadowBatchSourceChainCountByGroup.resize(
                newSize,
                0u);
        }
        if (m_virtualShadowBatchSourceGenerationByGroup[sourceGroup] !=
            m_virtualShadowBatchSourceGeneration) {
            const uint32_t chainOffset = static_cast<uint32_t>(
                m_virtualShadowMissingGroupsScratch.size());
            uint32_t groupIndex = sourceGroup;
            for (uint32_t hop = 0u; hop < 64u; ++hop) {
                if (groupIndex >=
                    m_virtualShadowResidencyGenerationByGroup.size()) {
                    m_virtualShadowResidencyGenerationByGroup.resize(
                        static_cast<size_t>(groupIndex) + 1u,
                        1u);
                }
                if (!IsGroupResident(groupIndex)) {
                    GetOrCreateVirtualShadowDependencyBucket(groupIndex);
                    const uint32_t dependencyBucketIndex =
                        static_cast<uint32_t>(
                            m_virtualShadowDependencyBucketIndexByGroup[
                                groupIndex]);
                    m_virtualShadowMissingGroupsScratch.push_back(
                        VirtualShadowMissingGroup{
                            groupIndex,
                            m_virtualShadowResidencyGenerationByGroup[
                                groupIndex],
                            dependencyBucketIndex});
                }
                uint32_t parentGroup = 0u;
                if (!TryGetCachedParentGroup(groupIndex, parentGroup) ||
                    parentGroup == groupIndex) {
                    break;
                }
                groupIndex = parentGroup;
            }
            m_virtualShadowBatchSourceGenerationByGroup[sourceGroup] =
                m_virtualShadowBatchSourceGeneration;
            m_virtualShadowBatchSourceChainOffsetByGroup[sourceGroup] =
                chainOffset;
            m_virtualShadowBatchSourceChainCountByGroup[sourceGroup] =
                static_cast<uint32_t>(
                    m_virtualShadowMissingGroupsScratch.size()) -
                chainOffset;
        }

        const uint32_t chainOffset =
            m_virtualShadowBatchSourceChainOffsetByGroup[sourceGroup];
        const uint32_t chainCount =
            m_virtualShadowBatchSourceChainCountByGroup[sourceGroup];
        VirtualShadowDependency dependency{
            CLodVirtualShadowPageToken{
                request.PhysicalPageIndex(),
                request.allocationGeneration,
                request.contentGeneration,
                request.ClipmapIndex(),
                request.VirtualAddress()},
            0u};
        for (uint32_t missingIndex = 0u;
             missingIndex < chainCount;
             ++missingIndex) {
            const auto& missing =
                m_virtualShadowMissingGroupsScratch[
                    chainOffset + missingIndex];
            auto& bucket = m_virtualShadowDependencyBuckets[
                missing.dependencyBucketIndex];
            dependency.residencyGeneration =
                missing.residencyGeneration;
            const bool inserted =
                InsertVirtualShadowDependency(bucket, dependency);
            if (inserted) {
                ++m_virtualShadowActiveDependencyPairCount;
            } else {
                ++m_virtualShadowUpgradeStats.dependenciesDeduplicated;
            }
            ++expandedPairCount;
        }
        m_virtualShadowUpgradeStats.dependenciesObserved +=
            chainCount;
    }

    m_virtualShadowUpgradeStats.activeDependencyGroups =
        static_cast<uint32_t>(m_virtualShadowDependencyBuckets.size());
    m_virtualShadowUpgradeStats.activeDependencyPairs =
        static_cast<uint32_t>(
            std::min<uint64_t>(
                m_virtualShadowActiveDependencyPairCount,
                UINT32_MAX));
    m_virtualShadowUpgradeStats.inputRecords += requests.size();
    m_virtualShadowUpgradeStats.uniqueInputRecords +=
        requests.size();
    m_virtualShadowUpgradeStats.expandedDependencyPairs +=
        expandedPairCount;
    m_virtualShadowUpgradeStats.peakActiveDependencyPairs = std::max(
        m_virtualShadowUpgradeStats.peakActiveDependencyPairs,
        m_virtualShadowUpgradeStats.activeDependencyPairs);
    TracyPlot(
        "CLodStreaming.VSMUpgrade.BatchInputs",
        static_cast<int64_t>(requests.size()));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.BatchAcceptedInputs",
        static_cast<int64_t>(requests.size()));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.BatchExpandedPairs",
        static_cast<int64_t>(expandedPairCount));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.ActiveDependencyPairs",
        static_cast<int64_t>(
            m_virtualShadowActiveDependencyPairCount));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.ActiveDependencySlots",
        static_cast<int64_t>(
            m_virtualShadowActiveDependencySlotCount));
    basic_telemetry::AddCounter(
        "CLod.VSM.InputRecords",
        requests.size());
    basic_telemetry::AddCounter(
        "CLod.VSM.ExpandedDependencyPairs",
        expandedPairCount);
    basic_telemetry::MaxGauge(
        "CLod.VSM.PeakActiveDependencyPairs",
        m_virtualShadowActiveDependencyPairCount);
    basic_telemetry::MaxGauge(
        "CLod.VSM.PeakActiveDependencySlots",
        m_virtualShadowActiveDependencySlotCount);
}

void CLodStreamingSystem::QueueVirtualShadowUpgradeForPromotion(uint32_t groupIndex) {
    ZoneScopedN("CLodStreamingSystem::QueueVirtualShadowUpgradeForPromotion");
    BASIC_TELEMETRY_SCOPE("CLod.VSM.Promotion");
    auto dependencies = RemoveVirtualShadowDependencyBucket(groupIndex);
    if (dependencies.empty()) {
        ++m_virtualShadowUpgradeStats.promotionsWithoutDependencies;
        return;
    }

    ++m_virtualShadowUpgradeStats.promotionsWithDependencies;
    uint64_t queuedCount = 0u;
    for (const auto& dependency : dependencies) {
        if (dependency.page.physicalPageIndex == UINT32_MAX) {
            continue;
        }
        QueueVirtualShadowReadyDependency(dependency);
        ++queuedCount;
    }
    m_virtualShadowUpgradeStats.eventsQueued += queuedCount;
    std::fill(
        dependencies.begin(),
        dependencies.end(),
        VirtualShadowDependency{});
    if (m_virtualShadowDependencyBucketPool.size() < 256u) {
        m_virtualShadowDependencyBucketPool.push_back(
            std::move(dependencies));
    }
}

void CLodStreamingSystem::PublishVirtualShadowUpgradeUpload() {
    ZoneScopedN("CLodStreamingSystem::PublishVirtualShadowUpgradeUpload");
    BASIC_TELEMETRY_SCOPE("CLod.VSM.PublishUpload");
    if (m_virtualShadowReadyTouchedPhysicalPages.empty() ||
        m_virtualShadowUpgradeUploadSlotCount == 0u) {
        return;
    }

    uint32_t slotIndex = UINT32_MAX;
    for (uint32_t candidate = 0u;
         candidate < m_virtualShadowUpgradeUploadSlotCount;
         ++candidate) {
        auto expected = VirtualShadowUpgradeUploadState::Free;
        if (m_virtualShadowUpgradeUploadSlots[candidate]->state.compare_exchange_strong(
                expected,
                VirtualShadowUpgradeUploadState::Filling,
                std::memory_order_acq_rel)) {
            slotIndex = candidate;
            break;
        }
    }
    if (slotIndex == UINT32_MAX) {
        TracyPlot("CLodStreaming.VSMUpgrade.UploadSlotStarved", int64_t{1});
        return;
    }

    auto& slot = *m_virtualShadowUpgradeUploadSlots[slotIndex];
    const uint64_t backingGeneration =
        slot.buffer ? slot.buffer->GetBackingGeneration() : 0u;
    if (slot.mapped != nullptr &&
        (backingGeneration == 0u ||
         slot.mappedBackingGeneration != backingGeneration)) {
        // Logical graph resources can survive a rebuild while their backing
        // allocations do not. A pointer returned by Map belongs to one
        // particular backing generation and must never cross rematerialization.
        slot.mapped = nullptr;
        slot.mappedBackingGeneration = 0u;
    }
    if (slot.mapped == nullptr && slot.buffer && slot.buffer->IsMaterialized()) {
        slot.buffer->GetAPIResource().Map(&slot.mapped, 0, 0);
        if (slot.mapped != nullptr) {
            slot.mappedBackingGeneration =
                slot.buffer->GetBackingGeneration();
        }
    }
    if (slot.mapped == nullptr) {
        slot.state.store(VirtualShadowUpgradeUploadState::Free, std::memory_order_release);
        return;
    }

    auto* outputs =
        static_cast<CLodVirtualShadowUpgradeInvalidationInput*>(slot.mapped);
    uint32_t outputCount = 0u;
    const size_t readyPageCount =
        m_virtualShadowReadyTouchedPhysicalPages.size();
    for (uint32_t physicalPageIndex :
         m_virtualShadowReadyTouchedPhysicalPages) {
        const auto& dependency =
            m_virtualShadowReadyByPhysicalPage[physicalPageIndex];
        CLodVirtualShadowUpgradeInvalidationInput input{};
        input.page = dependency.page;
        input.sourceGroupGlobalIndex = 0u;
        input.residencyGeneration = dependency.residencyGeneration;
        outputs[outputCount++] = input;
        m_virtualShadowReadyFlagsByPhysicalPage[physicalPageIndex] = 0u;
    }
    m_virtualShadowReadyTouchedPhysicalPages.clear();

    if (outputCount == 0u) {
        slot.state.store(VirtualShadowUpgradeUploadState::Free, std::memory_order_release);
        return;
    }
    slot.inputCount.store(outputCount, std::memory_order_relaxed);
    // No producer slot can be reused until all owners of this publication retire.
    auto slotOwner = m_virtualShadowUpgradeUploadSlots[slotIndex];
    auto lease = std::shared_ptr<const void>(slotOwner.get(),
        [slotOwner, wakeState = m_streamingWakeState](const void*) {
            slotOwner->inputCount.store(0u, std::memory_order_relaxed);
            slotOwner->state.store(VirtualShadowUpgradeUploadState::Free, std::memory_order_release);
            std::lock_guard lock(wakeState->mutex);
            if (wakeState->owner) wakeState->owner->RequestStreamingFrameWork();
        });
    slot.state.store(VirtualShadowUpgradeUploadState::Published, std::memory_order_release);
    m_virtualShadowUpgradeQueue.Enqueue({slot.buffer, outputCount, std::move(lease)});
    m_virtualShadowUpgradeStats.eventsUploaded += outputCount;
    TracyPlot("CLodStreaming.VSMUpgrade.PublishedInputs", static_cast<int64_t>(outputCount));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.ReadyPages",
        static_cast<int64_t>(readyPageCount));
    TracyPlot(
        "CLodStreaming.VSMUpgrade.PublishBytes",
        static_cast<int64_t>(
            outputCount *
            sizeof(CLodVirtualShadowUpgradeInvalidationInput)));
}

void CLodStreamingSystem::SetVirtualShadowUpgradeUploadBuffers(
    std::vector<std::shared_ptr<org::Buffer>> buffers) {
    const uint32_t count = std::min<uint32_t>(
        static_cast<uint32_t>(buffers.size()),
        VirtualShadowUpgradeUploadSlotCapacity);
    InvalidateVirtualShadowUpgradeUploadMappings();
    m_virtualShadowUpgradeQueue.DiscardUnsubmitted([](const auto&) { return true; });
    auto previous = m_virtualShadowUpgradeUploadSlots;
    m_virtualShadowUpgradeUploadSlotCount = count;
    for (uint32_t index = 0; index < VirtualShadowUpgradeUploadSlotCapacity; ++index) {
        std::shared_ptr<VirtualShadowUpgradeUploadSlot> retained;
        if (index < count) {
            for (const auto& slot : previous) {
                if (slot && slot->buffer == buffers[index]) { retained = slot; break; }
            }
        }
        if (!retained) {
            retained = std::make_shared<VirtualShadowUpgradeUploadSlot>();
            retained->buffer = index < count ? std::move(buffers[index]) : nullptr;
        }
        m_virtualShadowUpgradeUploadSlots[index] = std::move(retained);
    }
    RequestStreamingFrameWork();
}

void CLodStreamingSystem::InvalidateVirtualShadowUpgradeUploadMappings() {
    for (const auto& owner : m_virtualShadowUpgradeUploadSlots) {
        if (!owner) continue;
        auto& slot = *owner;
        if (slot.mapped != nullptr && slot.buffer &&
            slot.buffer->IsMaterialized() &&
            slot.mappedBackingGeneration ==
                slot.buffer->GetBackingGeneration()) {
            slot.buffer->GetAPIResource().Unmap(0, 0);
        }
        slot.mapped = nullptr;
        slot.mappedBackingGeneration = 0u;
    }
}

void CLodStreamingSystem::SetVirtualShadowFallbackFeedbackResources(
    std::shared_ptr<org::Buffer> dependencies,
    std::shared_ptr<org::Buffer> dependencyCount) {
    m_virtualShadowFallbackDependenciesBuffer = std::move(dependencies);
    m_virtualShadowFallbackDependencyCountBuffer = std::move(dependencyCount);
}

void CLodStreamingSystem::ClearVirtualShadowUpgradeState() {
    ZoneScopedN("CLodStreamingSystem::ClearVirtualShadowUpgradeState");
    uint64_t cleared = 0u;
    for (auto& bucket : m_virtualShadowDependencyBuckets) {
        cleared += bucket.dependencyCount;
        std::fill(
            bucket.dependencies.begin(),
            bucket.dependencies.end(),
            VirtualShadowDependency{});
        if (m_virtualShadowDependencyBucketPool.size() < 256u) {
            m_virtualShadowDependencyBucketPool.push_back(
                std::move(bucket.dependencies));
        }
    }
    cleared += m_virtualShadowReadyTouchedPhysicalPages.size();
    m_virtualShadowUpgradeStats.clearedDependencies += cleared;
    m_virtualShadowDependencyBuckets.clear();
    m_virtualShadowActiveDependencyPairCount = 0u;
    m_virtualShadowActiveDependencySlotCount = 0u;
    std::fill(
        m_virtualShadowDependencyBucketIndexByGroup.begin(),
        m_virtualShadowDependencyBucketIndexByGroup.end(),
        -1);
    m_virtualShadowDependencyBucketPool.clear();
    m_virtualShadowReadyByPhysicalPage.clear();
    m_virtualShadowReadyFlagsByPhysicalPage.clear();
    m_virtualShadowReadyTouchedPhysicalPages.clear();
    m_virtualShadowReadbackBatchScratch.clear();
    m_virtualShadowMissingGroupsScratch.clear();
    m_virtualShadowBatchSourceGenerationByGroup.clear();
    m_virtualShadowBatchSourceChainOffsetByGroup.clear();
    m_virtualShadowBatchSourceChainCountByGroup.clear();
    m_virtualShadowBatchSourceGeneration = 0u;
    // In-flight publications keep their slots and bytes until frame retirement.
    // Cancelled reservations cannot reappear as work for a newer graph generation.
    m_virtualShadowUpgradeQueue.DiscardUnsubmitted([](const auto&) { return true; });
    InvalidateVirtualShadowUpgradeUploadMappings();
    m_virtualShadowResidencyGenerationByGroup.clear();
}
