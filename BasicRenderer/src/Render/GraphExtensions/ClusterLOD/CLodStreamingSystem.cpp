#include "Render/GraphExtensions/ClusterLOD/CLodStreamingSystem.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <limits>
#include <numeric>
#include <span>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tracy/Tracy.hpp>

#include "Managers/Singletons/DeviceManager.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/ViewManager.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingBeginFramePass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingFeedbackSortPass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodStreamingReadbackCopyPass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodAsyncUploadPass.h"
#include "Render/GraphExtensions/ClusterLOD/CLodDirectStorageLaunchPass.h"
#include "Render/Runtime/UploadServiceAccess.h"
#include "Managers/UploadInstance.h"
#include "Interfaces/IDynamicDeclaredResources.h"
#include "Render/MemoryIntrospectionAPI.h"
#include "Render/Runtime/OpenRenderGraphSettings.h"
#include "RenderPasses/StreamingUploadPass.h"
#include "Resources/Resolvers/ResourceGroupResolver.h"
#include "Resources/Buffers/DynamicBuffer.h"
#include "Telemetry/NvPerfIntegration.h"
#include "Mesh/ClusterLODShaderTypes.h"
#include "BuiltinResources.h"

namespace {
uint64_t ClodDiagNowMs()
{
    using Clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

bool NvPerfCaptureSuppressesCLodService()
{
    static bool logged = false;
    const bool suppressed = br::telemetry::nvperf::CaptureActive();
    if (suppressed && !logged) {
        logged = true;
        spdlog::info("CLod streaming: suspending streaming service while NVPerf capture is active");
    }
    return suppressed;
}

bool NvPerfCaptureSuppressesCLodReadback()
{
    static bool logged = false;
    const bool suppressed =
        br::telemetry::nvperf::CaptureActive() ||
        br::telemetry::nvperf::StreamingSuppressed();
    if (suppressed && !logged) {
        logged = true;
        spdlog::info("CLod streaming: suppressing new streaming feedback readbacks for deterministic sampling");
    }
    return suppressed;
}
}

struct CLodStreamingSystem::ParallelSortState {
    std::shared_ptr<Buffer> keyScratch;
    std::shared_ptr<Buffer> payloadScratch;
    std::shared_ptr<Buffer> sumTable;
    std::shared_ptr<Buffer> reduceTable;
    std::shared_ptr<Buffer> constants;
    std::shared_ptr<Buffer> countScatterArgs;
    std::shared_ptr<Buffer> reduceScanArgs;
};

namespace {
    constexpr uint64_t kInvalidCLodMeshPageKey = (std::numeric_limits<uint64_t>::max)();

    struct CLodStreamingUploadSnapshotKey {
        uint64_t dstResourceId = 0;
        uint64_t srcResourceId = 0;
        size_t dstOffset = 0;
        size_t srcOffset = 0;
        size_t size = 0;

        bool operator==(const CLodStreamingUploadSnapshotKey&) const = default;
    };

    std::vector<CLodStreamingUploadSnapshotKey> MakeStreamingUploadSnapshotKey(
        const std::vector<StreamingUploadDescriptor>& uploads) {
        std::vector<CLodStreamingUploadSnapshotKey> key;
        key.reserve(uploads.size());
        for (const auto& upload : uploads) {
            key.push_back({
                upload.dstResource ? upload.dstResource->GetGlobalResourceID() : 0ull,
                upload.srcUploadBuffer ? upload.srcUploadBuffer->GetGlobalResourceID() : 0ull,
                upload.dstOffset,
                upload.srcOffset,
                upload.size,
            });
        }
        return key;
    }

    class CLodStructuralStreamingUploadPass final : public CopyPass, public IDynamicDeclaredResources, public IHasImmediateModeCommands {
    public:
        using ConsumeUploadsFn = std::function<std::vector<StreamingUploadDescriptor>()>;
        using MakePoolResolverFn = std::function<std::unique_ptr<IResourceResolver>()>;

        CLodStructuralStreamingUploadPass(
            ConsumeUploadsFn consumeUploads,
            MakePoolResolverFn makePoolResolver)
            : m_consumeUploads(std::move(consumeUploads))
            , m_makePoolResolver(std::move(makePoolResolver)) {}

        bool DeclaredResourcesChanged() const override {
            ZoneScopedN("CLodStructuralStreamingUploadPass::DeclaredResourcesChanged::SnapshotOnly");

            if (!m_uploadSnapshotValid) {
                m_uploadSnapshot = m_consumeUploads ? m_consumeUploads() : std::vector<StreamingUploadDescriptor>{};
                m_uploadSnapshotValid = true;
            }

            StreamingUploadInputs nextInputs{};
            nextInputs.uploads = m_uploadSnapshot;
            if (!nextInputs.uploads.empty() && m_makePoolResolver) {
                nextInputs.poolResolver = m_makePoolResolver();
            }

            auto nextKey = MakeStreamingUploadSnapshotKey(nextInputs.uploads);
            const bool changed = !m_initialized || nextKey != m_snapshotKey;
            m_initialized = true;
            m_snapshotKey = std::move(nextKey);
            m_inputs = std::move(nextInputs);
            return changed;
        }

        void DeclareResourceUsages(CopyPassBuilder* builder) override {
            builder->PreferQueue(QueueKind::Copy);
        }

        void Setup() override {}

        void RecordImmediateCommands(ImmediateExecutionContext& context) override {
            for (const auto& upload : m_inputs.uploads) {
                if (!upload.dstResource || !upload.srcUploadBuffer || upload.size == 0) {
                    continue;
                }
                context.list.CopyBufferRegion(
                    upload.dstResource.get(), upload.dstOffset,
                    upload.srcUploadBuffer, upload.srcOffset,
                    upload.size);
            }
            m_uploadSnapshot.clear();
            m_uploadSnapshotValid = false;
        }

        PassReturn Execute(PassExecutionContext&) override { return {}; }
        void Cleanup() override {}

    private:
        ConsumeUploadsFn m_consumeUploads;
        MakePoolResolverFn m_makePoolResolver;
        mutable std::vector<StreamingUploadDescriptor> m_uploadSnapshot;
        mutable StreamingUploadInputs m_inputs;
        mutable std::vector<CLodStreamingUploadSnapshotKey> m_snapshotKey;
        mutable bool m_uploadSnapshotValid = false;
        mutable bool m_initialized = false;
    };

    struct CLodAsyncUploadSnapshot {
        std::vector<std::shared_ptr<CLodUploadBatch>> batches;
        std::vector<std::shared_ptr<Resource>> destinations;
        rhi::Timeline completionTimeline;
        uint64_t completionValue = 0;
    };

    class CLodStructuralAsyncUploadPass final : public CopyPass, public IDynamicDeclaredResources, public IHasImmediateModeCommands {
    public:
        using TryAcquireSnapshotFn = std::function<bool(CLodAsyncUploadSnapshot&)>;
        using SubmitSnapshotFn = std::function<PassReturn(CLodAsyncUploadSnapshot&)>;

        CLodStructuralAsyncUploadPass(
            TryAcquireSnapshotFn tryAcquireSnapshot,
            SubmitSnapshotFn submitSnapshot,
            std::unique_ptr<IResourceResolver> poolResolver)
            : m_tryAcquireSnapshot(std::move(tryAcquireSnapshot))
            , m_submitSnapshot(std::move(submitSnapshot))
            , m_poolResolver(std::move(poolResolver)) {}

        bool DeclaredResourcesChanged() const override {
            ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::SnapshotOnly");

            CancelClaimedSnapshot();
            CLodAsyncUploadSnapshot nextSnapshot{};
            bool armed = false;
            {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::AcquireSnapshot");
                armed = m_tryAcquireSnapshot && m_tryAcquireSnapshot(nextSnapshot);
            }
            std::vector<uint64_t> nextDestinationIds;
            if (armed) {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::BuildDestinationIds");
                nextDestinationIds.reserve(nextSnapshot.destinations.size());
                for (const auto& destination : nextSnapshot.destinations) {
                    nextDestinationIds.push_back(destination ? destination->GetGlobalResourceID() : 0ull);
                }
            }

            bool changed = false;
            {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::InstallSnapshot");
                changed = !m_initialized || nextDestinationIds != m_declaredDestinationIds;
                m_initialized = true;
                m_declaredDestinationIds = std::move(nextDestinationIds);
                m_armed = armed;
                m_snapshot = std::move(nextSnapshot);
            }
            return changed;
        }

        void DeclareResourceUsages(CopyPassBuilder* builder) override {
            ZoneScopedN("CLodStructuralAsyncUploadPass::DeclareResourceUsages");
            for (auto& destination : m_snapshot.destinations) {
                if (destination) {
                    builder->WithCopyDest(destination);
                }
            }
            if (m_poolResolver) {
                builder->WithCopyDest(*m_poolResolver);
            }
            builder->PreferQueue(QueueKind::Graphics);
        }

        void Setup() override {}

        void RecordImmediateCommands(ImmediateExecutionContext& context) override {
            ZoneScopedN("CLodStructuralAsyncUploadPass::RecordImmediateCommands");
            if (m_armed) {
                for (const auto& batch : m_snapshot.batches) {
                    if (!batch) continue;
                    for (const auto& copy : batch->copies) {
                        if (copy.destination && copy.staging && copy.size != 0u) {
                            context.list.CopyBufferRegion(
                                copy.destination, copy.destinationOffset,
                                copy.staging, copy.stagingOffset,
                                copy.size);
                        }
                    }
                }
            }
        }

        PassReturn Execute(PassExecutionContext&) override {
            ZoneScopedN("CLodStructuralAsyncUploadPass::Execute");
            if (!m_armed || !m_submitSnapshot) {
                return {};
            }
            m_armed = false;
            return m_submitSnapshot(m_snapshot);
        }
        void Cleanup() override { CancelClaimedSnapshot(); }

    private:
        TryAcquireSnapshotFn m_tryAcquireSnapshot;
        SubmitSnapshotFn m_submitSnapshot;
        std::unique_ptr<IResourceResolver> m_poolResolver;
        mutable CLodAsyncUploadSnapshot m_snapshot;
        mutable std::vector<uint64_t> m_declaredDestinationIds;
        mutable bool m_armed = false;
        mutable bool m_initialized = false;

        void CancelClaimedSnapshot() const {
            for (const auto& batch : m_snapshot.batches) {
                if (!batch || !batch->ticket) continue;
                auto expected = CLodUploadTicketState::Claimed;
                batch->ticket->state.compare_exchange_strong(
                    expected,
                    CLodUploadTicketState::Cancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            }
            m_snapshot = {};
            m_armed = false;
        }
    };

    struct CLodStreamingReadbackSnapshot {
        CLodStreamingReadbackCopyInputs inputs;
        std::shared_ptr<Buffer> counterStaging;
        std::shared_ptr<Buffer> requestsStaging;
        std::shared_ptr<Buffer> usedGroupsCounterStaging;
        std::shared_ptr<Buffer> usedGroupsBufferStaging;
        std::shared_ptr<Buffer> sourceGroupMismatchCounterStaging;
        std::shared_ptr<Buffer> sourceGroupMismatchDetailsStaging;
        uint32_t selectedSlot = UINT32_MAX;
    };

    class CLodStructuralStreamingReadbackCopyPass final : public CopyPass, public IDynamicDeclaredResources, public IHasImmediateModeCommands {
    public:
        using TryAcquireSnapshotFn = std::function<bool(CLodStreamingReadbackSnapshot&)>;
        using CompleteSnapshotFn = std::function<PassReturn(uint32_t)>;
        using CancelSnapshotFn = std::function<void(uint32_t)>;

        CLodStructuralStreamingReadbackCopyPass(
            TryAcquireSnapshotFn tryAcquireSnapshot,
            CompleteSnapshotFn completeSnapshot,
            CancelSnapshotFn cancelSnapshot)
            : m_tryAcquireSnapshot(std::move(tryAcquireSnapshot))
            , m_completeSnapshot(std::move(completeSnapshot))
            , m_cancelSnapshot(std::move(cancelSnapshot)) {}

        bool DeclaredResourcesChanged() const override {
            ZoneScopedN("CLodStructuralStreamingReadbackCopyPass::DeclaredResourcesChanged::SnapshotOnly");

            CancelArmedSnapshot();
            CLodStreamingReadbackSnapshot nextSnapshot{};
            const bool armed = m_tryAcquireSnapshot && m_tryAcquireSnapshot(nextSnapshot);
            std::vector<uint64_t> nextKey;
            if (armed) {
                nextKey = {
                    nextSnapshot.inputs.counterSource ? nextSnapshot.inputs.counterSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.requestsSource ? nextSnapshot.inputs.requestsSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.usedGroupsCounterSource ? nextSnapshot.inputs.usedGroupsCounterSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.usedGroupsBufferSource ? nextSnapshot.inputs.usedGroupsBufferSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.sourceGroupMismatchCounterSource ? nextSnapshot.inputs.sourceGroupMismatchCounterSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.sourceGroupMismatchDetailsSource ? nextSnapshot.inputs.sourceGroupMismatchDetailsSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.counterStaging ? nextSnapshot.counterStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.requestsStaging ? nextSnapshot.requestsStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.usedGroupsCounterStaging ? nextSnapshot.usedGroupsCounterStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.usedGroupsBufferStaging ? nextSnapshot.usedGroupsBufferStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.sourceGroupMismatchCounterStaging ? nextSnapshot.sourceGroupMismatchCounterStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.sourceGroupMismatchDetailsStaging ? nextSnapshot.sourceGroupMismatchDetailsStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.selectedSlot,
                };
            }

            const bool changed = !m_initialized || nextKey != m_snapshotKey;
            m_initialized = true;
            m_armed = armed;
            m_snapshot = std::move(nextSnapshot);
            m_snapshotKey = std::move(nextKey);
            return changed;
        }

        void DeclareResourceUsages(CopyPassBuilder* builder) override {
            if (m_armed) {
                builder->WithCopySource(
                    m_snapshot.inputs.counterSource,
                    m_snapshot.inputs.requestsSource,
                    m_snapshot.inputs.usedGroupsCounterSource,
                    m_snapshot.inputs.usedGroupsBufferSource);
                if (m_snapshot.inputs.sourceGroupMismatchCounterSource) {
                    builder->WithCopySource(m_snapshot.inputs.sourceGroupMismatchCounterSource);
                }
                if (m_snapshot.inputs.sourceGroupMismatchDetailsSource) {
                    builder->WithCopySource(m_snapshot.inputs.sourceGroupMismatchDetailsSource);
                }
                builder->WithCopyDest(
                    m_snapshot.counterStaging,
                    m_snapshot.requestsStaging,
                    m_snapshot.usedGroupsCounterStaging,
                    m_snapshot.usedGroupsBufferStaging);
                if (m_snapshot.sourceGroupMismatchCounterStaging) {
                    builder->WithCopyDest(m_snapshot.sourceGroupMismatchCounterStaging);
                }
                if (m_snapshot.sourceGroupMismatchDetailsStaging) {
                    builder->WithCopyDest(m_snapshot.sourceGroupMismatchDetailsStaging);
                }
            }
            builder->PreferQueue(QueueKind::Graphics);
        }

        void Setup() override {}

        void RecordImmediateCommands(ImmediateExecutionContext& context) override {
            if (!m_armed) {
                return;
            }

            CopyWholeBuffer(context, m_snapshot.counterStaging, m_snapshot.inputs.counterSource);
            CopyWholeBuffer(context, m_snapshot.requestsStaging, m_snapshot.inputs.requestsSource);
            CopyWholeBuffer(context, m_snapshot.usedGroupsCounterStaging, m_snapshot.inputs.usedGroupsCounterSource);
            CopyWholeBuffer(context, m_snapshot.usedGroupsBufferStaging, m_snapshot.inputs.usedGroupsBufferSource);
            CopyWholeBuffer(context, m_snapshot.sourceGroupMismatchCounterStaging, m_snapshot.inputs.sourceGroupMismatchCounterSource);
            CopyWholeBuffer(context, m_snapshot.sourceGroupMismatchDetailsStaging, m_snapshot.inputs.sourceGroupMismatchDetailsSource);
        }

        PassReturn Execute(PassExecutionContext&) override {
            if (!m_armed || !m_completeSnapshot) {
                return {};
            }
            PassReturn ret = m_completeSnapshot(m_snapshot.selectedSlot);
            m_armed = false;
            return ret;
        }

        void Cleanup() override { CancelArmedSnapshot(); }

    private:
        static void CopyWholeBuffer(
            ImmediateExecutionContext& context,
            const std::shared_ptr<Buffer>& staging,
            const std::shared_ptr<Buffer>& source) {
            if (!source || !staging) {
                return;
            }

            uint64_t bytes = 0;
            if (source->TryGetBufferByteSize(bytes) && bytes > 0) {
                context.list.CopyBufferRegion(staging, 0, source.get(), 0, bytes);
            }
        }

        TryAcquireSnapshotFn m_tryAcquireSnapshot;
        CompleteSnapshotFn m_completeSnapshot;
        CancelSnapshotFn m_cancelSnapshot;
        mutable CLodStreamingReadbackSnapshot m_snapshot;
        mutable std::vector<uint64_t> m_snapshotKey;
        mutable bool m_armed = false;
        mutable bool m_initialized = false;

        void CancelArmedSnapshot() const {
            if (m_armed && m_cancelSnapshot && m_snapshot.selectedSlot != UINT32_MAX) {
                m_cancelSnapshot(m_snapshot.selectedSlot);
            }
            m_armed = false;
            m_snapshot = {};
        }
    };

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
        const MeshManager::CLodGroupStreamingInfo& info,
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
        const MeshManager::CLodGroupStreamingInfo& info,
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
        for (const MeshManager::CLodGroupStreamingInfo::ReferencedPageSegment& referencedSegment
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

CLodStreamingSystem::CLodStreamingSystem() {
    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };

    m_streamingNonResidentBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), ~0u);
    m_streamingNonResidentBitsDirtyWordFlags.assign(m_streamingNonResidentBitsCpu.size(), 0u);
    m_streamingActiveGroupsBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_streamingPinnedGroupsBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_streamingResidencyInitializedBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_groupLastUsedTick.assign(m_streamingStorageGroupCapacity, 0u);
    m_streamingRequestStateByGroup.assign(m_streamingStorageGroupCapacity, StreamingRequestState::None);
    m_pendingLoadPriorityByGroup.assign(m_streamingStorageGroupCapacity, 0u);
    m_pendingStreamingRequestHeapIndexByGroup.assign(m_streamingStorageGroupCapacity, UINT32_MAX);
    m_pendingStreamingRequestGenerationByGroup.assign(m_streamingStorageGroupCapacity, 0u);
    m_streamingDiagnosticsByGroup.resize(m_streamingStorageGroupCapacity);
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();

    try {
        auto getter = SettingsManager::GetInstance().getSettingGetter<std::function<MeshManager*()>>(CLodStreamingMeshManagerGetterSettingName);
        m_getMeshManager = getter();
    }
    catch (...) {
    }

    try {
        auto getFramesInFlight = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight");
        m_streamingReadbackRingSize = std::max<uint32_t>(getFramesInFlight(), 1u);
    }
    catch (...) {
        m_streamingReadbackRingSize = 3u;
    }

    try {
        m_getStreamingCpuUploadBudgetRequests =
            SettingsManager::GetInstance().getSettingGetter<uint32_t>(CLodStreamingCpuUploadBudgetSettingName);
        m_streamingCpuUploadBudgetRequests = std::max(m_getStreamingCpuUploadBudgetRequests(), 1u);
    }
    catch (...) {
        m_getStreamingCpuUploadBudgetRequests = {};
        m_streamingCpuUploadBudgetRequests = 10000u;
    }

    m_streamingNonResidentBits = CreateAliasedUnmaterializedStructuredBuffer(
        CLodBitsetWordCount(m_streamingStorageGroupCapacity),
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_streamingNonResidentBits->SetName("CLod Streaming NonResident Bits");
    tagBufferUsage(m_streamingNonResidentBits, "Cluster LOD streaming");

    m_streamingActiveGroupsBits = CreateAliasedUnmaterializedStructuredBuffer(
        CLodBitsetWordCount(m_streamingStorageGroupCapacity),
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_streamingActiveGroupsBits->SetName("CLod Streaming Active Groups Bits");
    tagBufferUsage(m_streamingActiveGroupsBits, "Cluster LOD streaming");

    m_streamingLoadRequests = CreateAliasedUnmaterializedStructuredBuffer(
        CLodStreamingRequestCapacity,
        sizeof(CLodStreamingRequest),
        true,
        false,
        false,
        false);
    m_streamingLoadRequests->SetName("CLod Streaming Load Requests");
    tagBufferUsage(m_streamingLoadRequests, "Cluster LOD streaming");

    m_streamingLoadRequestKeys = CreateAliasedUnmaterializedStructuredBuffer(
        CLodStreamingRequestCapacity,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_streamingLoadRequestKeys->SetName("CLod Streaming Load Request Keys");
    tagBufferUsage(m_streamingLoadRequestKeys, "Cluster LOD streaming");

    m_streamingLoadCounter = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_streamingLoadCounter->SetName("CLod Streaming Load Counter");
    tagBufferUsage(m_streamingLoadCounter, "Cluster LOD streaming");

    m_streamingRuntimeState = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(CLodStreamingRuntimeState), true, false, false, false);
    m_streamingRuntimeState->SetName("CLod Streaming Runtime State");
    tagBufferUsage(m_streamingRuntimeState, "Cluster LOD streaming");

    m_usedGroupsCounter = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_usedGroupsCounter->SetName("CLod Used Groups Counter");
    tagBufferUsage(m_usedGroupsCounter, "Cluster LOD streaming");

    m_usedGroupsBuffer = CreateAliasedUnmaterializedStructuredBuffer(
        CLodUsedGroupsCapacity,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_usedGroupsBuffer->SetName("CLod Used Groups Buffer");
    tagBufferUsage(m_usedGroupsBuffer, "Cluster LOD streaming");

    m_sourceGroupMismatchCounter = CreateAliasedUnmaterializedStructuredBuffer(1, sizeof(uint32_t), true, false, false, false);
    m_sourceGroupMismatchCounter->SetName("CLod Source Group Mismatch Counter");
    tagBufferUsage(m_sourceGroupMismatchCounter, "Cluster LOD diagnostics");

    m_sourceGroupMismatchDetails = CreateAliasedUnmaterializedStructuredBuffer(
        CLodSourceGroupMismatchDetailCapacity,
        sizeof(CLodSourceGroupMismatchDetail),
        true,
        false,
        false,
        false);
    m_sourceGroupMismatchDetails->SetName("CLod Source Group Mismatch Details");
    tagBufferUsage(m_sourceGroupMismatchDetails, "Cluster LOD diagnostics");

    // Self-managed readback pipeline
    {
        auto device = DeviceManager::GetInstance().GetDevice();
        auto result = device.CreateTimeline(m_streamingReadbackFencePtr, 0, "CLodStreamingReadbackFence");
        if (result == rhi::Result::Ok && m_streamingReadbackFencePtr) {
            m_streamingReadbackFenceHandle = m_streamingReadbackFencePtr.Get();
        }
        result = device.CreateTimeline(m_streamingUploadCompletionFencePtr, 0, "CLodStreamingUploadCompletionFence");
        if (result == rhi::Result::Ok && m_streamingUploadCompletionFencePtr) {
            m_streamingUploadCompletionFenceHandle = m_streamingUploadCompletionFencePtr.Get();
        }
        result = device.CreateTimeline(m_directStorageLaunchFencePtr, 0, "CLodDirectStorageLaunchFence");
        if (result == rhi::Result::Ok && m_directStorageLaunchFencePtr) {
            m_directStorageLaunchFenceHandle = m_directStorageLaunchFencePtr.Get();
        }
    }

    const uint64_t counterStagingBytes = sizeof(uint32_t);
    const uint64_t requestsStagingBytes = static_cast<uint64_t>(CLodStreamingRequestCapacity) * sizeof(CLodStreamingRequest);
    const uint64_t usedGroupsCounterStagingBytes = sizeof(uint32_t);
    const uint64_t usedGroupsBufferStagingBytes = static_cast<uint64_t>(CLodUsedGroupsCapacity) * sizeof(uint32_t);
    m_readbackStagingSlots.resize(m_streamingReadbackRingSize);
    for (uint32_t i = 0; i < m_streamingReadbackRingSize; ++i) {
        auto& slot = m_readbackStagingSlots[i];
        slot.counterStaging = Buffer::CreateShared(rhi::HeapType::Readback, counterStagingBytes);
        slot.counterStaging->SetName(("CLodReadbackCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.counterStaging, "Cluster LOD streaming readback");
        slot.requestsStaging = Buffer::CreateShared(rhi::HeapType::Readback, requestsStagingBytes);
        slot.requestsStaging->SetName(("CLodReadbackRequests_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.requestsStaging, "Cluster LOD streaming readback");
        slot.usedGroupsCounterStaging = Buffer::CreateShared(rhi::HeapType::Readback, usedGroupsCounterStagingBytes);
        slot.usedGroupsCounterStaging->SetName(("CLodReadbackUsedGroupsCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.usedGroupsCounterStaging, "Cluster LOD streaming readback");
        slot.usedGroupsBufferStaging = Buffer::CreateShared(rhi::HeapType::Readback, usedGroupsBufferStagingBytes);
        slot.usedGroupsBufferStaging->SetName(("CLodReadbackUsedGroupsBuffer_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.usedGroupsBufferStaging, "Cluster LOD streaming readback");
        const uint64_t sourceGroupMismatchCounterStagingBytes = sizeof(uint32_t);
        const uint64_t sourceGroupMismatchDetailsStagingBytes =
            static_cast<uint64_t>(CLodSourceGroupMismatchDetailCapacity) * sizeof(CLodSourceGroupMismatchDetail);
        slot.sourceGroupMismatchCounterStaging = Buffer::CreateShared(rhi::HeapType::Readback, sourceGroupMismatchCounterStagingBytes);
        slot.sourceGroupMismatchCounterStaging->SetName(("CLodReadbackSourceGroupMismatchCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.sourceGroupMismatchCounterStaging, "Cluster LOD diagnostics readback");
        slot.sourceGroupMismatchDetailsStaging = Buffer::CreateShared(rhi::HeapType::Readback, sourceGroupMismatchDetailsStagingBytes);
        slot.sourceGroupMismatchDetailsStaging->SetName(("CLodReadbackSourceGroupMismatchDetails_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.sourceGroupMismatchDetailsStaging, "Cluster LOD diagnostics readback");
    }

    StartStreamingWorker();
}

CLodStreamingSystem::~CLodStreamingSystem() {
    Shutdown();
    DestroyParallelSortResources();
}

void CLodStreamingSystem::ShutdownGraphResources() {
    StopStreamingWorker();

    if (MeshManager* meshManager = m_getMeshManager ? m_getMeshManager() : nullptr) {
        ClearStreamingUploadFunction(meshManager);
    }
    for (auto& slot : m_readbackStagingSlots) {
        slot.state.store(ReadbackStagingSlot::State::Free, std::memory_order_relaxed);
        slot.fenceValue = 0;
    }
    m_readbackStagingCursor = 0;
    m_streamingReadbackDiscardedFenceCounter.store(
        m_streamingReadbackFenceCounter.load(std::memory_order_acquire),
        std::memory_order_release);
}

void CLodStreamingSystem::Shutdown() {
    const bool wasAlreadyQuitting = m_streamingWorkerQuit.load(std::memory_order_acquire);
    if (!wasAlreadyQuitting && m_getMeshManager) {
        if (MeshManager* meshManager = m_getMeshManager()) {
            const auto [pendingLaunches, pendingUploads] = meshManager->GetPendingCLodDirectStorageCounts();
            if (pendingLaunches != 0u || pendingUploads != 0u) {
                spdlog::warn(
                    "CLod streaming shutdown with pending DirectStorage work: launches={} uploads={}",
                    pendingLaunches,
                    pendingUploads);
            }
        }
    }
    StopStreamingWorker();

    // Destruction transfers streaming ownership back to MeshManager/PagePool.
    // A render-graph registry reset does not: the same streaming system remains
    // alive and must retain its resident pages across the graph rebuild.
    ResetStreamingStateForShutdown();
}

void CLodStreamingSystem::OnRegistryReset(ResourceRegistry* reg) {
    (void)reg;
    StopStreamingWorker();

    // The registry and the alias placements are graph-local, but CLod residency
    // is not. Keep the CPU domain, page ownership, resident groups, LRU and
    // pending disk work intact while releasing only graph-facing backings.
    auto releaseBufferBacking = [](const std::shared_ptr<Buffer>& buffer) {
        if (buffer) {
            buffer->Dematerialize();
        }
    };

    releaseBufferBacking(m_streamingNonResidentBits);
    releaseBufferBacking(m_streamingActiveGroupsBits);
    releaseBufferBacking(m_streamingLoadRequestKeys);
    releaseBufferBacking(m_streamingLoadRequests);
    releaseBufferBacking(m_streamingLoadCounter);
    releaseBufferBacking(m_streamingRuntimeState);
    releaseBufferBacking(m_usedGroupsCounter);
    releaseBufferBacking(m_usedGroupsBuffer);
    releaseBufferBacking(m_sourceGroupMismatchCounter);
    releaseBufferBacking(m_sourceGroupMismatchDetails);
    if (m_parallelSortState) {
        releaseBufferBacking(m_parallelSortState->keyScratch);
        releaseBufferBacking(m_parallelSortState->payloadScratch);
        releaseBufferBacking(m_parallelSortState->sumTable);
        releaseBufferBacking(m_parallelSortState->reduceTable);
        releaseBufferBacking(m_parallelSortState->constants);
        releaseBufferBacking(m_parallelSortState->countScatterArgs);
        releaseBufferBacking(m_parallelSortState->reduceScanArgs);
    }

    if (MeshManager* meshManager = m_getMeshManager ? m_getMeshManager() : nullptr) {
        ClearStreamingUploadFunction(meshManager);
    }
    // The rematerialized bitsets have undefined contents. Re-upload the
    // authoritative CPU mirrors without declaring every live group nonresident.
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();
    m_publishedActiveGroupsBits.clear();
    m_publishedActiveGroupScanCount = 0u;
    m_publishedActiveGroupsBitsUploadPending = true;
    m_activeGroupsSnapshotQueue.Reset();
    m_retainedActiveGroupsSnapshot.reset();
    m_streamingServicePublishedGeneration = 0;

    // Readbacks reference graph queue timelines/backings and cannot cross a
    // rebuild. Their decoded requests are advisory and will be regenerated.
    m_decodedReadbackBatch.clear();
    m_decodedUsedGroupsBatch.clear();
    for (auto& slot : m_readbackStagingSlots) {
        slot.state.store(ReadbackStagingSlot::State::Free, std::memory_order_relaxed);
        slot.fenceValue = 0;
    }
    m_readbackStagingCursor = 0;
    m_streamingReadbackDiscardedFenceCounter.store(
        m_streamingReadbackFenceCounter.load(std::memory_order_acquire),
        std::memory_order_release);
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
}

void CLodStreamingSystem::ResetStreamingStateForShutdown() {
    StopStreamingWorker();

    auto releaseBufferBacking = [](const std::shared_ptr<Buffer>& buffer) {
        if (buffer) {
            buffer->Dematerialize();
        }
    };

    releaseBufferBacking(m_streamingNonResidentBits);
    releaseBufferBacking(m_streamingActiveGroupsBits);
    releaseBufferBacking(m_streamingLoadRequestKeys);
    releaseBufferBacking(m_streamingLoadRequests);
    releaseBufferBacking(m_streamingLoadCounter);
    releaseBufferBacking(m_streamingRuntimeState);
    releaseBufferBacking(m_usedGroupsCounter);
    releaseBufferBacking(m_usedGroupsBuffer);
    releaseBufferBacking(m_sourceGroupMismatchCounter);
    releaseBufferBacking(m_sourceGroupMismatchDetails);
    if (m_parallelSortState) {
        releaseBufferBacking(m_parallelSortState->keyScratch);
        releaseBufferBacking(m_parallelSortState->payloadScratch);
        releaseBufferBacking(m_parallelSortState->sumTable);
        releaseBufferBacking(m_parallelSortState->reduceTable);
        releaseBufferBacking(m_parallelSortState->constants);
        releaseBufferBacking(m_parallelSortState->countScatterArgs);
        releaseBufferBacking(m_parallelSortState->reduceScanArgs);
    }
    MeshManager* meshManager = nullptr;
    if (m_getMeshManager) {
        meshManager = m_getMeshManager();
    }

    if (meshManager != nullptr) {
        ClearStreamingUploadFunction(meshManager);
        meshManager->InvalidateCLodDiskStreamingPipeline();
    }
    if (m_uploadStream) {
        m_uploadStream->Cleanup();
        m_uploadStream.reset();
    }
    m_uploadBatchQueue.Reset();
    m_retainedUploadBatch.reset();
    m_outstandingUploadBatches.clear();

    // Evict ALL resident groups so MeshManager clears groupResidentFlags,
    // zeroes GroupChunks counts, and wipes GroupPageMap entries. Without this,
    // the GPU reads stale chunk data with freed-page references after rebuild.
    std::vector<uint32_t> ownedGroups;
    ownedGroups.reserve(m_groupOwnedPages.size());
    for (const auto& [groupIndex, _] : m_groupOwnedPages) {
        ownedGroups.push_back(groupIndex);
    }
    for (uint32_t groupIndex : ownedGroups) {
        if (meshManager != nullptr && IsGroupResident(groupIndex)) {
            meshManager->EvictCLodGroupResidency(groupIndex, true);
        }
        ReleaseOwnedPagesForGroup(groupIndex, meshManager);
    }

    if (meshManager != nullptr) {
        for (const auto& [_, pages] : m_preAllocatedPagesByGroup) {
            ReleasePreAllocatedPages(pages, meshManager);
        }

		// Full extension shutdown happens only after Renderer::StallPipeline, so no
		// command list can still reference these pages. Release pinned resident and
		// retiring pages directly instead of putting them through the normal
		// frame-delayed retirement path: the latter is cleared just below and used to
		// strand every pinned page whenever a recipe replacement recreated CLod.
		std::vector<uint32_t> pinnedPagesToFree;
		pinnedPagesToFree.reserve(m_pagePinnedStorage.size());
		for (uint32_t page = 0; page < static_cast<uint32_t>(m_pagePinnedStorage.size()); ++page) {
			if (m_pagePinnedStorage[page] != 0u) {
				pinnedPagesToFree.push_back(page);
				m_pagePinnedStorage[page] = 0u;
			}
		}
		if (!pinnedPagesToFree.empty()) {
			if (PagePool* pool = meshManager->GetCLodPagePool()) {
				pool->FreePinnedPages(pinnedPagesToFree);
				spdlog::info(
					"CLod streaming shutdown returned {} pinned pages to the shared page pool",
					pinnedPagesToFree.size());
			}
		}
    }

    m_pendingStreamingRequests.clear();
    m_pageLru.Clear();
    m_pageOwnerGroup.clear();
    m_pageOwnerSegment.clear();
    m_pageOwnerMeshPageKey.clear();
    m_pageReuseRequiresNonResidentEpoch.clear();
    m_pageReuseNonResidentQueuedTick.clear();
    m_pageReuseUploadFenceValue.clear();
    m_retiringPhysicalPages.clear();
    m_retiringPagesAwaitingUploadFence.clear();
    m_pageResidentGroups.clear();
    m_pageProtectedThisUpdate.clear();
    m_pagesProtectedThisUpdate.clear();
    m_pageRetireAfterTick.clear();
    m_pageRetirePinned.clear();
    m_pagePinnedStorage.clear();
    m_groupOwnedPages.clear();
    m_groupOwnedMeshPageKeys.clear();
    m_groupCommittedPageMaps.clear();
    m_residentMeshPageToPhysicalPage.clear();
    m_residentMeshPageRefCounts.clear();
    m_pendingMeshPageToPhysicalPage.clear();
    m_pendingMeshPageRefCounts.clear();
    m_pendingResidencyUploadFenceByGroup.clear();
    m_residencyGroupsAwaitingUploadFence.clear();
    for (uint32_t word : m_protectedGroupWordsScratch) {
        if (word < m_protectedGroupsBitsScratch.size()) {
            m_protectedGroupsBitsScratch[word] = 0u;
        }
    }
    m_protectedGroupWordsScratch.clear();
    ClearPrefetchedChildLayouts();
    m_preAllocatedPagesByGroup.clear();
    m_readyStreamingCompletionsByGroup.clear();
    m_pendingResidencyCommitGroups.clear();
    m_groupsUsingPinnedStorage.clear();
    m_usedGroupsWordsCpu.clear();
    std::fill(m_parentGroupByGroup.begin(), m_parentGroupByGroup.end(), UINT32_MAX);
    m_pageLruInitialized = false;
    m_streamingResidentGroupsCount = 0u;
    std::fill(m_streamingNonResidentBitsCpu.begin(), m_streamingNonResidentBitsCpu.end(), ~0u);
    std::fill(m_streamingNonResidentBitsDirtyWordFlags.begin(), m_streamingNonResidentBitsDirtyWordFlags.end(), 0u);
    m_streamingNonResidentBitsDirtyWords.clear();
    m_streamingNonResidentBitsDirtyWordCursor = 0u;
    m_streamingNonResidentBitsDirtyWordsSorted = true;
    std::fill(m_streamingActiveGroupsBitsCpu.begin(), m_streamingActiveGroupsBitsCpu.end(), 0u);
    std::fill(m_streamingPinnedGroupsBitsCpu.begin(), m_streamingPinnedGroupsBitsCpu.end(), 0u);
    std::fill(m_streamingResidencyInitializedBitsCpu.begin(), m_streamingResidencyInitializedBitsCpu.end(), 0u);
    std::fill(m_groupLastUsedTick.begin(), m_groupLastUsedTick.end(), 0u);
    std::fill(m_streamingRequestStateByGroup.begin(), m_streamingRequestStateByGroup.end(), StreamingRequestState::None);
    std::fill(m_pendingLoadPriorityByGroup.begin(), m_pendingLoadPriorityByGroup.end(), 0u);
    std::fill(m_pendingStreamingRequestHeapIndexByGroup.begin(), m_pendingStreamingRequestHeapIndexByGroup.end(), UINT32_MAX);
    std::fill(m_pendingStreamingRequestGenerationByGroup.begin(), m_pendingStreamingRequestGenerationByGroup.end(), 0u);
    std::fill(m_streamingDiagnosticsByGroup.begin(), m_streamingDiagnosticsByGroup.end(), StreamingDiagnosticsRecord{});
    m_streamingDiagnosticsDecodedRequestsThisFrame = 0u;
    m_streamingDiagnosticsQueuedLoadRequestsThisFrame = 0u;
    m_streamingDiagnosticsDuplicateRequestsThisFrame = 0u;
    m_streamingDiagnosticsPreallocationDeferralsThisFrame = 0u;
    m_streamingDiagnosticsPromotionDeferralsThisFrame = 0u;
    m_streamingDiagnosticsCompletionSuccessThisFrame = 0u;
    m_streamingDiagnosticsCompletionFailedThisFrame = 0u;
    m_streamingDiagnosticsUploadQueuedGroupsThisFrame = 0u;
    m_streamingDiagnosticsUploadQueuedBytesThisFrame = 0u;
    m_streamingDiagnosticsRequestToUploadSamplesThisFrame = 0u;
    m_streamingDiagnosticsRequestToUploadSumThisFrame = 0u;
    m_streamingDiagnosticsRequestToUploadWorstThisFrame = 0u;
    m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame = 0u;
    m_streamingDiagnosticsRequestToResidentSamplesThisFrame = 0u;
    m_streamingDiagnosticsRequestToResidentSumThisFrame = 0u;
    m_streamingDiagnosticsRequestToResidentWorstThisFrame = 0u;
    m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame = 0u;
    m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame = 0u;
    m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame = 0u;
    m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame = 0u;
    m_streamingDiagnosticsUploadToResidentSamplesThisFrame = 0u;
    m_streamingDiagnosticsUploadToResidentSumThisFrame = 0u;
    m_streamingDiagnosticsUploadToResidentWorstThisFrame = 0u;
    m_streamingDiagnosticsCommitToResidentSamplesThisFrame = 0u;
    m_streamingDiagnosticsCommitToResidentSumThisFrame = 0u;
    m_streamingDiagnosticsCommitToResidentWorstThisFrame = 0u;
    m_streamingRequestsInProgressCount = 0u;
    m_pendingStreamingRequestCount = 0u;
    m_streamingDiagnosticTick = 0;
    m_streamingResidencyMutationEpoch = 0;
    m_streamingNonResidentBitsQueuedEpoch = 0;
    m_streamingNonResidentBitsQueuedTick = 0;
    m_streamingNonResidentBitsUploadFenceEpoch = 0;
    m_streamingNonResidentBitsUploadFenceValue = 0;
    m_streamingActiveGroupScanCount = 0u;
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();
    m_publishedActiveGroupsBits.clear();
    m_publishedActiveGroupScanCount = 0u;
    m_publishedActiveGroupsBitsUploadPending = true;
    m_activeGroupsSnapshotQueue.Reset();
    m_retainedActiveGroupsSnapshot.reset();
    m_streamingServicePublishedGeneration = 0;
    m_streamingDomainEventScratch.clear();
    m_childGroupsScratch.clear();
    m_lastStreamingDomainEventGeneration = 0;
    m_streamingDomainFullResetPending = true;

    // Discard any stale decoded readback data from the worker thread.
    m_decodedReadbackBatch.clear();
    m_decodedUsedGroupsBatch.clear();

    // Clear in-flight flags so the worker thread doesn't process stale readback data.
    {
        for (auto& slot : m_readbackStagingSlots) {
        slot.state.store(ReadbackStagingSlot::State::Free, std::memory_order_relaxed);
            slot.fenceValue = 0;
        }
        m_readbackStagingCursor = 0;
    }
    m_streamingReadbackDiscardedFenceCounter.store(
        m_streamingReadbackFenceCounter.load(std::memory_order_acquire),
        std::memory_order_release);
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    m_streamingServiceEpoch.notify_one();
}

void CLodStreamingSystem::Initialize(RenderGraph& rg) {
    StopStreamingWorker();
    MeshManager* meshManager = m_getMeshManager ? m_getMeshManager() : nullptr;
    if (meshManager != nullptr) {
        ClearStreamingUploadFunction(meshManager);
    }
    // Create a dedicated copy queue for async CLod streaming uploads.
    m_uploadQueueSlot = rg.CreateQueue(
        QueueKind::Copy,
        "CLodAsyncUpload",
        QueueAutoAssignmentPolicy::ManualOnly);

    if (!m_uploadStream) {
        m_uploadStream = std::make_unique<CLodUploadStream>();
    }

    EnsureParallelSortResources();
    if (meshManager != nullptr && m_pageLruInitialized) {
        InstallStreamingUploadFunction(meshManager);
    }

    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    ++m_uploadBatchGeneration;
    StartStreamingWorker();
}

void CLodStreamingSystem::StartStreamingWorker() {
    if (m_streamingWorkerThread.joinable()) return;
    m_streamingWorkerQuit.store(false, std::memory_order_release);
    m_streamingWorkerThread = std::thread(&CLodStreamingSystem::StreamingWorkerMain, this);
}

void CLodStreamingSystem::StopStreamingWorker() {
    m_streamingWorkerQuit.store(true, std::memory_order_release);
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    m_streamingServiceEpoch.notify_all();
    if (m_streamingWorkerThread.joinable()) {
        m_streamingWorkerThread.join();
    }
}

void CLodStreamingSystem::ClearStreamingUploadFunction(MeshManager* meshManager) {
    if (meshManager == nullptr) {
        return;
    }

    meshManager->SetCLodStreamingUploadFunction({});
    if (PagePool* pool = meshManager->GetCLodPagePool()) {
        pool->SetUploadFunction({});
    }
}

void CLodStreamingSystem::InstallStreamingUploadFunction(MeshManager* meshManager) {
    if (meshManager == nullptr || m_uploadStream == nullptr) {
        return;
    }

    PagePool* pool = meshManager->GetCLodPagePool();
    if (pool == nullptr) {
        return;
    }

    auto uploadFn = [stream = m_uploadStream.get()](
        const void* data, size_t size,
        rg::runtime::UploadTarget target, size_t offset) {
        if (target.kind == rg::runtime::UploadTarget::Kind::PinnedShared) {
            if (auto dynamicBuffer = std::dynamic_pointer_cast<DynamicBuffer>(target.pinned)) {
                dynamicBuffer->RetainExternalUpload(data, size, offset);
            }
        }
        stream->UploadData(data, size, std::move(target), offset);
    };
    pool->SetUploadFunction(uploadFn);
    meshManager->SetCLodStreamingUploadFunction(std::move(uploadFn));
}

void CLodStreamingSystem::RequestStreamingFrameWork() {
    ZoneScopedN("CLodStreamingSystem::RequestStreamingFrameWork");
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    m_streamingServiceEpoch.notify_one();
}

void CLodStreamingSystem::PublishStreamingFrameWorkForFrame() {
    ZoneScopedN("CLodStreamingSystem::PublishStreamingFrameWorkForFrame");
    (void)PublishPendingStreamingStorageGpuResizeLocked();

    CLodActiveGroupsSnapshot newest;
    bool received = false;
    m_activeGroupsSnapshotQueue.Drain([&](CLodActiveGroupsSnapshot&& snapshot) {
        newest = std::move(snapshot);
        received = true;
    });
    if (received) {
        m_publishedActiveGroupsBits = std::move(newest.bits);
        m_publishedActiveGroupScanCount = newest.activeGroupScanCount;
        m_publishedActiveGroupsBitsUploadPending = true;
        m_streamingServicePublishedGeneration = newest.generation;
    }
}

void CLodStreamingSystem::PublishActiveGroupSnapshot() {
    if (m_retainedActiveGroupsSnapshot) {
        if (!m_activeGroupsSnapshotQueue.TryPush(std::move(*m_retainedActiveGroupsSnapshot))) {
            if (m_streamingActiveGroupsBitsUploadPending) {
                const uint32_t capacity =
                    m_streamingGpuStorageGroupCapacity.load(std::memory_order_acquire);
                const uint32_t wordCount = CLodBitsetWordCount(capacity);
                m_retainedActiveGroupsSnapshot->bits.assign(
                    m_streamingActiveGroupsBitsCpu.begin(),
                    m_streamingActiveGroupsBitsCpu.begin() + std::min<uint32_t>(
                        wordCount, static_cast<uint32_t>(m_streamingActiveGroupsBitsCpu.size())));
                m_retainedActiveGroupsSnapshot->activeGroupScanCount =
                    std::min(m_streamingActiveGroupScanCount, capacity);
                m_retainedActiveGroupsSnapshot->generation = ++m_streamingServicePublishedGeneration;
                m_streamingActiveGroupsBitsUploadPending = false;
            }
            return;
        }
        m_retainedActiveGroupsSnapshot.reset();
    }
    if (!m_streamingActiveGroupsBitsUploadPending) return;

    CLodActiveGroupsSnapshot snapshot;
    const uint32_t wordCount = CLodBitsetWordCount(m_streamingGpuStorageGroupCapacity);
    snapshot.bits.assign(
        m_streamingActiveGroupsBitsCpu.begin(),
        m_streamingActiveGroupsBitsCpu.begin() + std::min<uint32_t>(
            wordCount, static_cast<uint32_t>(m_streamingActiveGroupsBitsCpu.size())));
    snapshot.activeGroupScanCount = std::min(
        m_streamingActiveGroupScanCount,
        m_streamingGpuStorageGroupCapacity.load(std::memory_order_acquire));
    snapshot.generation = ++m_streamingServicePublishedGeneration;
    m_streamingActiveGroupsBitsUploadPending = false;
    if (!m_activeGroupsSnapshotQueue.TryPush(std::move(snapshot))) {
        // Latest-value traffic is deliberately coalesced. This retained value
        // is replaced below on the next dirty publication if the queue remains full.
        m_retainedActiveGroupsSnapshot = std::move(snapshot);
    }
    TracyPlot("CLodStreaming.ActiveSnapshotDepth", static_cast<int64_t>(m_activeGroupsSnapshotQueue.Depth()));
}

void CLodStreamingSystem::RunStreamingServiceWork() {
    ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork");
    if (NvPerfCaptureSuppressesCLodService()) {
        return;
    }

    if (!PublishRetainedUploadBatch()) {
        return; // lossless backpressure: do not accept more completions yet
    }
    ObserveUploadBatchTickets();
    if (m_retainedUploadBatch) return;
    const uint64_t resizeAckGeneration =
        m_streamingGpuResizeAckGeneration.load(std::memory_order_acquire);
    if (resizeAckGeneration != m_observedStreamingGpuResizeAckGeneration) {
        m_observedStreamingGpuResizeAckGeneration = resizeAckGeneration;
        MarkStreamingNonResidentBitsDirtyAll();
        MarkStreamingActiveGroupsBitsDirty();
    }
    MeshManager* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::GetMeshManager");
        if (m_getMeshManager) {
            meshManager = m_getMeshManager();
        }
    }
    if (m_uploadStream == nullptr) {
        return;
    }

    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::ProcessStreamingDomainEvents");
        ProcessStreamingDomainEvents();
    }
    if (meshManager != nullptr) {
        {
            ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::PollCompletedReadbackSlots");
            PollCompletedReadbackSlots();
        }
        {
            ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::ProcessStreamingRequestsBudgeted");
            ProcessStreamingRequestsBudgeted();
        }
    }
    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::QueuePendingNonResidentBitsUpload");
        QueuePendingNonResidentBitsUpload();
    }
    SealStreamingUploadBatch();
    PublishActiveGroupSnapshot();

    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::PublishTelemetry");
        TracyPlot("CLodStreaming.Service.PendingDecodedGroups", static_cast<int64_t>(m_readbackBatchScratch.size()));
        TracyPlot("CLodStreaming.Service.PendingCpuRequests", static_cast<int64_t>(m_pendingStreamingRequestCount));
    }

}

void CLodStreamingSystem::GatherStructuralPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses) {
    rg.RegisterResource(Builtin::CLod::StreamingNonResidentBits, m_streamingNonResidentBits);
    rg.RegisterResource(Builtin::CLod::StreamingActiveGroupsBits, m_streamingActiveGroupsBits);
    rg.RegisterResource(Builtin::CLod::StreamingLoadRequestKeys, m_streamingLoadRequestKeys);
    rg.RegisterResource(Builtin::CLod::StreamingLoadRequests, m_streamingLoadRequests);
    rg.RegisterResource(Builtin::CLod::StreamingLoadCounter, m_streamingLoadCounter);
    rg.RegisterResource(Builtin::CLod::StreamingRuntimeState, m_streamingRuntimeState);
    rg.RegisterResource(Builtin::CLod::StreamingTouchedGroupsCounter, m_usedGroupsCounter);
    rg.RegisterResource(Builtin::CLod::StreamingTouchedGroups, m_usedGroupsBuffer);

    auto makePoolResolver = [this]() -> std::unique_ptr<IResourceResolver> {
        MeshManager* mm = m_getMeshManager ? m_getMeshManager() : nullptr;
        if (!mm) {
            return nullptr;
        }
        PagePool* pool = mm->GetCLodPagePool();
        if (!pool) {
            return nullptr;
        }

        auto slabGroup = pool->GetSlabResourceGroup();
        if (auto pt = pool->GetPageTableBuffer()) {
            slabGroup->AddResource(pt);
        }
        return std::make_unique<ResourceGroupResolver>(slabGroup);
    };

    auto streamingUploadInsertPoint =
        RenderGraph::ExternalInsertPoint::After("EvaluateMaterialGroupsPass");
    streamingUploadInsertPoint.AlsoBefore("GTAOFilterPass");
    streamingUploadInsertPoint.AlsoBefore("DeferredShadingPass");
    streamingUploadInsertPoint.keepExtensionOrder = false;
    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Copy(
            "CLod::StreamingUpload",
            std::make_shared<CLodStructuralStreamingUploadPass>(
                []() {
                    return rg::runtime::ConsumeStreamingUploadsDispatch();
                },
                makePoolResolver))
            .At(std::move(streamingUploadInsertPoint))
            .PreferQueue(QueueKind::Copy));

    auto asyncUploadInsertPoint =
        RenderGraph::ExternalInsertPoint::After("EvaluateMaterialGroupsPass");
    asyncUploadInsertPoint.AlsoBefore("GTAOFilterPass");
    asyncUploadInsertPoint.AlsoBefore("DeferredShadingPass");
    asyncUploadInsertPoint.keepExtensionOrder = false;
    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Copy(
            "CLod::AsyncUpload",
            std::make_shared<CLodStructuralAsyncUploadPass>(
                [this](CLodAsyncUploadSnapshot& outSnapshot) -> bool {
                    ZoneScopedN("CLodAsyncUpload::AcquireSnapshot");
                    if (!m_uploadStream || !m_streamingUploadCompletionFenceHandle.IsValid()) {
                        return false;
                    }
                    std::shared_ptr<CLodUploadBatch> batch;
                    m_uploadBatchQueue.Drain([&](std::shared_ptr<CLodUploadBatch>&& candidate) {
                        if (!candidate || !candidate->ticket) return;
                        auto expected = CLodUploadTicketState::Published;
                        if (!candidate->ticket->state.compare_exchange_strong(
                                expected, CLodUploadTicketState::Claimed,
                                std::memory_order_acq_rel, std::memory_order_acquire)) {
                            return;
                        }
                        outSnapshot.batches.push_back(std::move(candidate));
                    });
                    if (outSnapshot.batches.empty()) return false;

                    outSnapshot.completionTimeline = m_streamingUploadCompletionFenceHandle;
                    std::unordered_set<uint64_t> seen;
                    for (const auto& claimed : outSnapshot.batches) {
                        for (const auto& destination : claimed->destinations) {
                            if (destination && seen.insert(destination->GetGlobalResourceID()).second) {
                                outSnapshot.destinations.push_back(destination);
                            }
                        }
                    }
                    TracyPlot("CLodAsyncUpload.BatchQueueDepth", static_cast<int64_t>(m_uploadBatchQueue.Depth()));
                    return true;
                },
                [this](CLodAsyncUploadSnapshot& snapshot) -> PassReturn {
                    PassReturn result{};
                    if (snapshot.batches.empty() || !snapshot.completionTimeline.IsValid()) return result;
                    const uint64_t completionValue =
                        m_streamingUploadCompletionFenceCounter.fetch_add(1, std::memory_order_relaxed) + 1u;
                    snapshot.completionValue = completionValue;
                    for (const auto& batch : snapshot.batches) {
                        if (!batch || !batch->ticket) continue;
                        batch->ticket->completionValue.store(completionValue, std::memory_order_relaxed);
                        batch->ticket->state.store(CLodUploadTicketState::Submitted, std::memory_order_release);
                    }
                    result.externalSignalsAfterCompletion.push_back({snapshot.completionTimeline, completionValue});
                    RequestStreamingFrameWork();
                    return result;
                },
                makePoolResolver()))
            .At(std::move(asyncUploadInsertPoint))
            .PreferQueue(QueueKind::Copy)
            .PinToQueue(m_uploadQueueSlot));

	auto streamingBeginPass = std::make_shared<CLodStreamingBeginFramePass>(
		[]() -> UploadInstance* { return nullptr; },
		m_streamingLoadCounter,
        m_streamingLoadRequestKeys,
		m_usedGroupsCounter,
        m_sourceGroupMismatchCounter,
		m_streamingNonResidentBits,
		m_streamingActiveGroupsBits,
		m_streamingRuntimeState,
		[](std::vector<uint32_t>& outBits, uint32_t& outFirstWord, UploadInstance*) {
            // Non-resident data is sealed by the single streaming writer into
            // the same ticketed batch as page payload and page-map changes.
            outBits.clear();
            outFirstWord = 0u;
            return false;
        },
		[this](std::vector<uint32_t>& outBits, uint32_t& outActiveScanCount) {
            outActiveScanCount = m_publishedActiveGroupScanCount;
            if (!m_publishedActiveGroupsBitsUploadPending) {
                outBits.clear();
                return false;
            }

            outBits = m_publishedActiveGroupsBits;
            m_publishedActiveGroupsBitsUploadPending = false;
            return true;
		},
        [this]() {
            PublishStreamingFrameWorkForFrame();
        },
        [this]() {
            RequestStreamingFrameWork();
        });

    auto streamingBeginPassDesc = RenderGraph::ExternalPassDesc::Compute(
        "CLod::StreamingBeginFramePass",
        streamingBeginPass);
    // Keep the CLod front-end behind the visibility/depth clear so the graph
    // cannot legally sink ClearVisibilityBufferPass after CLod rasterization.
    auto streamingBeginInsertPoint =
        RenderGraph::ExternalInsertPoint::After("ClearVisibilityBufferPass");
    streamingBeginInsertPoint.keepExtensionOrder = false;
    streamingBeginPassDesc.At(std::move(streamingBeginInsertPoint));
    outPasses.push_back(std::move(streamingBeginPassDesc));
}

void CLodStreamingSystem::GatherStructuralTailPasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses) {
    (void)rg;
    const bool hasStreamingFeedbackSort = EnsureParallelSortResources();
    if (hasStreamingFeedbackSort) {
        auto feedbackSortPass = std::make_shared<CLodStreamingFeedbackSortPass>(
            m_streamingLoadRequestKeys,
            m_streamingLoadRequests,
            m_streamingLoadCounter,
            m_parallelSortState->keyScratch,
            m_parallelSortState->payloadScratch,
            m_parallelSortState->sumTable,
            m_parallelSortState->reduceTable,
            m_parallelSortState->constants,
            m_parallelSortState->countScatterArgs,
            m_parallelSortState->reduceScanArgs);

        outPasses.push_back(
            RenderGraph::ExternalPassDesc::Compute(
                "CLod::StreamingFeedbackSort",
                feedbackSortPass)
                .At(RenderGraph::ExternalInsertPoint::After("PresentPass"))
                .PreferQueue(QueueKind::Graphics));
    }

    auto readbackPass = std::make_shared<CLodStructuralStreamingReadbackCopyPass>(
        [this](CLodStreamingReadbackSnapshot& snapshot) -> bool {
            if (NvPerfCaptureSuppressesCLodReadback()) {
                return false;
            }
            if (!m_streamingReadbackFenceHandle.IsValid() || m_readbackStagingSlots.empty()) {
                return false;
            }

            uint32_t selectedSlot = UINT32_MAX;
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_readbackStagingSlots.size()); ++i) {
                const uint32_t idx =
                    (m_readbackStagingCursor + i) % static_cast<uint32_t>(m_readbackStagingSlots.size());
                auto expected = ReadbackStagingSlot::State::Free;
                if (m_readbackStagingSlots[idx].state.compare_exchange_strong(
                        expected, ReadbackStagingSlot::State::Recording,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    selectedSlot = idx;
                    break;
                }
            }

            if (selectedSlot == UINT32_MAX) {
                TracyPlot("CLodStreaming.ReadbackSlotFullEvents", static_cast<int64_t>(++m_readbackSlotFullEvents));
                return false;
            }

            m_readbackStagingCursor =
                (selectedSlot + 1u) % static_cast<uint32_t>(m_readbackStagingSlots.size());
            auto& selected = m_readbackStagingSlots[selectedSlot];
            selected.fenceValue = 0;

            auto& slot = m_readbackStagingSlots[selectedSlot];
            snapshot.inputs.counterSource = m_streamingLoadCounter;
            snapshot.inputs.requestsSource = m_streamingLoadRequests;
            snapshot.inputs.usedGroupsCounterSource = m_usedGroupsCounter;
            snapshot.inputs.usedGroupsBufferSource = m_usedGroupsBuffer;
            snapshot.inputs.sourceGroupMismatchCounterSource = m_sourceGroupMismatchCounter;
            snapshot.inputs.sourceGroupMismatchDetailsSource = m_sourceGroupMismatchDetails;
            snapshot.counterStaging = slot.counterStaging;
            snapshot.requestsStaging = slot.requestsStaging;
            snapshot.usedGroupsCounterStaging = slot.usedGroupsCounterStaging;
            snapshot.usedGroupsBufferStaging = slot.usedGroupsBufferStaging;
            snapshot.sourceGroupMismatchCounterStaging = slot.sourceGroupMismatchCounterStaging;
            snapshot.sourceGroupMismatchDetailsStaging = slot.sourceGroupMismatchDetailsStaging;
            snapshot.selectedSlot = selectedSlot;
            return true;
        },
        [this](uint32_t selectedSlot) -> PassReturn {
            if (!m_streamingReadbackFenceHandle.IsValid()) {
                return {};
            }

            if (selectedSlot >= m_readbackStagingSlots.size()) return {};

            auto& armedSlot = m_readbackStagingSlots[selectedSlot];
            if (armedSlot.state.load(std::memory_order_acquire) != ReadbackStagingSlot::State::Recording) {
                return {};
            }

            const uint64_t fenceValue = m_streamingReadbackFenceCounter.fetch_add(1, std::memory_order_relaxed) + 1;
            armedSlot.fenceValue = fenceValue;
            armedSlot.state.store(ReadbackStagingSlot::State::Submitted, std::memory_order_release);
            RequestStreamingFrameWork();
            return { m_streamingReadbackFenceHandle, fenceValue };
        },
        [this](uint32_t selectedSlot) {
            if (selectedSlot >= m_readbackStagingSlots.size()) return;
            auto expected = ReadbackStagingSlot::State::Recording;
            m_readbackStagingSlots[selectedSlot].state.compare_exchange_strong(
                expected, ReadbackStagingSlot::State::Free,
                std::memory_order_acq_rel, std::memory_order_acquire);
        });

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Copy(
            "CLod::StreamingReadbackCopy",
            readbackPass)
            .At(RenderGraph::ExternalInsertPoint::After(
                hasStreamingFeedbackSort ? "CLod::StreamingFeedbackSort" : "PresentPass"))
            .PreferQueue(QueueKind::Graphics));

    CLodDirectStorageLaunchInputs launchInputs{};
    if (m_getMeshManager) {
        if (MeshManager* meshManager = m_getMeshManager()) {
            if (PagePool* pool = meshManager->GetCLodPagePool()) {
                auto slabGroup = pool->GetSlabResourceGroup();
                if (auto pageTable = pool->GetPageTableBuffer()) {
                    slabGroup->AddResource(pageTable);
                }
                launchInputs.targetSlabResolver = std::make_unique<ResourceGroupResolver>(slabGroup);
            }
        }
    }
    launchInputs.launchCallback = [this]() -> PassReturn {
        ZoneScopedN("CLodDirectStorageLaunch::PassRecordCallback");
        if (!m_directStorageLaunchFenceHandle.IsValid()) {
            return {};
        }

        if (m_directStorageArmedLaunchFenceValue.load(std::memory_order_acquire) != 0u ||
            !m_directStorageLaunchRequested.exchange(false, std::memory_order_acq_rel)) {
            return {};
        }

        {
            ZoneScopedN("CLodDirectStorageLaunch::PassRecordCallback::ArmFence");
            const uint64_t fenceValue =
                m_directStorageLaunchFenceCounter.fetch_add(1, std::memory_order_relaxed) + 1u;
            m_directStorageArmedLaunchFenceValue.store(fenceValue, std::memory_order_release);

            PassReturn result{};
            result.externalSignalsAfterCompletion.push_back({ m_directStorageLaunchFenceHandle, fenceValue });
            return result;
        }
    };

    outPasses.push_back(
        RenderGraph::ExternalPassDesc::Copy(
            "CLod::DirectStorageLaunch",
            std::make_shared<CLodDirectStorageLaunchPass>(std::move(launchInputs)))
            .At(RenderGraph::ExternalInsertPoint::After("PresentPass"))
            .PreferQueue(QueueKind::Graphics));
}

void CLodStreamingSystem::GatherFramePasses(RenderGraph& rg, std::vector<RenderGraph::ExternalPassDesc>& outPasses) {
    (void)rg;
    PublishStreamingFrameWorkForFrame();

}

uint32_t CLodStreamingSystem::BitWordAddress(uint32_t key) {
    return key >> 5u;
}

uint32_t CLodStreamingSystem::BitMask(uint32_t key) {
    return 1u << (key & 31u);
}

bool CLodStreamingSystem::EnsureParallelSortResources() {
    if (m_parallelSortAttempted) {
        return m_parallelSortAvailable;
    }

    m_parallelSortAttempted = true;
    m_parallelSortState = std::make_unique<ParallelSortState>();

    auto tagBufferUsage = [](const std::shared_ptr<Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            rg::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };

    constexpr uint32_t blockSize = 4u * 128u;
    constexpr uint32_t sortBinCount = 16u;
    constexpr uint32_t numBlocks = (CLodStreamingRequestCapacity + blockSize - 1u) / blockSize;
    constexpr uint32_t numReducedBlocks = (numBlocks + blockSize - 1u) / blockSize;
    constexpr uint32_t sumTableElements = sortBinCount * numBlocks;
    constexpr uint32_t reduceTableElements = sortBinCount * numReducedBlocks;
    constexpr uint32_t radixIterationCount = 8u;

    m_parallelSortState->keyScratch = CreateAliasedUnmaterializedStructuredBuffer(
        CLodStreamingRequestCapacity,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_parallelSortState->keyScratch->SetName("CLod Streaming Sort Key Scratch");

    m_parallelSortState->payloadScratch = CreateAliasedUnmaterializedStructuredBuffer(
        CLodStreamingRequestCapacity,
        sizeof(CLodStreamingRequest),
        true,
        false,
        false,
        false);
    m_parallelSortState->payloadScratch->SetName("CLod Streaming Sort Payload Scratch");

    m_parallelSortState->sumTable = CreateAliasedUnmaterializedStructuredBuffer(
        sumTableElements,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_parallelSortState->sumTable->SetName("CLod Streaming Sort Sum Table");

    m_parallelSortState->reduceTable = CreateAliasedUnmaterializedStructuredBuffer(
        reduceTableElements,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_parallelSortState->reduceTable->SetName("CLod Streaming Sort Reduce Table");

    struct ParallelSortConstantsCpu {
        uint32_t numKeys;
        int32_t numBlocksPerThreadGroup;
        uint32_t numThreadGroups;
        uint32_t numThreadGroupsWithAdditionalBlocks;
        uint32_t numReduceThreadgroupPerBin;
        uint32_t numScanValues;
        uint32_t shift;
        uint32_t padding;
    };
    static_assert(sizeof(ParallelSortConstantsCpu) == 32u);

    m_parallelSortState->constants = CreateAliasedUnmaterializedStructuredBuffer(
        radixIterationCount,
        sizeof(ParallelSortConstantsCpu),
        true,
        false,
        false,
        false);
    m_parallelSortState->constants->SetName("CLod Streaming Sort Constants");

    m_parallelSortState->countScatterArgs = CreateAliasedUnmaterializedStructuredBuffer(
        3u,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_parallelSortState->countScatterArgs->SetName("CLod Streaming Sort Count Scatter Args");

    m_parallelSortState->reduceScanArgs = CreateAliasedUnmaterializedStructuredBuffer(
        3u,
        sizeof(uint32_t),
        true,
        false,
        false,
        false);
    m_parallelSortState->reduceScanArgs->SetName("CLod Streaming Sort Reduce Scan Args");

    tagBufferUsage(m_parallelSortState->keyScratch, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->payloadScratch, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->sumTable, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->reduceTable, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->constants, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->countScatterArgs, "Cluster LOD streaming sort");
    tagBufferUsage(m_parallelSortState->reduceScanArgs, "Cluster LOD streaming sort");

    m_parallelSortAvailable = true;
    return true;
}

void CLodStreamingSystem::DestroyParallelSortResources() {
    if (!m_parallelSortState) {
        m_parallelSortAvailable = false;
        return;
    }

    m_parallelSortState.reset();
    m_parallelSortAvailable = false;
}

void CLodStreamingSystem::MarkStreamingNonResidentBitsDirtyWord(uint32_t wordAddress) {
    if (wordAddress >= m_streamingNonResidentBitsCpu.size()) {
        return;
    }

    if (m_streamingNonResidentBitsDirtyWordFlags.size() < m_streamingNonResidentBitsCpu.size()) {
        m_streamingNonResidentBitsDirtyWordFlags.resize(m_streamingNonResidentBitsCpu.size(), 0u);
    }
    if (m_streamingNonResidentBitsDirtyWordFlags[wordAddress] == 0u) {
        m_streamingNonResidentBitsDirtyWordFlags[wordAddress] = 1u;
        if (!m_streamingNonResidentBitsDirtyWords.empty() &&
            wordAddress < m_streamingNonResidentBitsDirtyWords.back()) {
            m_streamingNonResidentBitsDirtyWordsSorted = false;
        }
        m_streamingNonResidentBitsDirtyWords.push_back(wordAddress);
    }

    if (!m_streamingNonResidentBitsUploadPending) {
        m_streamingNonResidentBitsDirtyBegin = wordAddress;
        m_streamingNonResidentBitsDirtyEnd = wordAddress + 1u;
        m_streamingNonResidentBitsUploadPending = true;
        return;
    }

    m_streamingNonResidentBitsDirtyBegin = std::min(m_streamingNonResidentBitsDirtyBegin, wordAddress);
    m_streamingNonResidentBitsDirtyEnd = std::max(m_streamingNonResidentBitsDirtyEnd, wordAddress + 1u);
}

void CLodStreamingSystem::MarkStreamingNonResidentBitsDirtyAll() {
    const auto wordCount = static_cast<uint32_t>(m_streamingNonResidentBitsCpu.size());
    if (wordCount == 0u) {
        m_streamingNonResidentBitsUploadPending = false;
        m_streamingNonResidentBitsDirtyBegin = 0u;
        m_streamingNonResidentBitsDirtyEnd = 0u;
        m_streamingNonResidentBitsDirtyWords.clear();
        m_streamingNonResidentBitsDirtyWordCursor = 0u;
        m_streamingNonResidentBitsDirtyWordFlags.clear();
        m_streamingNonResidentBitsDirtyWordsSorted = true;
        return;
    }

    m_streamingNonResidentBitsDirtyWordFlags.assign(wordCount, 1u);
    m_streamingNonResidentBitsDirtyWords.resize(wordCount);
    m_streamingNonResidentBitsDirtyWordCursor = 0u;
    std::iota(
        m_streamingNonResidentBitsDirtyWords.begin(),
        m_streamingNonResidentBitsDirtyWords.end(),
        0u);
    m_streamingNonResidentBitsDirtyWordsSorted = true;
    m_streamingNonResidentBitsUploadPending = true;
    m_streamingNonResidentBitsDirtyBegin = 0u;
    m_streamingNonResidentBitsDirtyEnd = wordCount;
}

bool CLodStreamingSystem::TryConsumeStreamingNonResidentBitsUpload(
    std::vector<uint32_t>& outBits,
    uint32_t& outFirstWord,
    uint32_t maxWords) {
    ZoneScopedN("CLodStreamingSystem::TryConsumeNonResidentBitsUpload");
    outBits.clear();
    outFirstWord = 0u;
    if (!m_streamingNonResidentBitsUploadPending || maxWords == 0u) {
        return false;
    }

    const uint32_t gpuWordCount = CLodBitsetWordCount(m_streamingGpuStorageGroupCapacity);
    const uint32_t cpuWordCount = static_cast<uint32_t>(m_streamingNonResidentBitsCpu.size());
    const uint32_t validWordCount = std::min(gpuWordCount, cpuWordCount);
    if (validWordCount == 0u) {
        if (m_pendingStreamingGpuStorageGroupCapacity == 0u) {
            m_streamingNonResidentBitsUploadPending = false;
            m_streamingNonResidentBitsDirtyBegin = 0u;
            m_streamingNonResidentBitsDirtyEnd = 0u;
            m_streamingNonResidentBitsDirtyWords.clear();
            m_streamingNonResidentBitsDirtyWordCursor = 0u;
        }
        return false;
    }

    if (m_streamingNonResidentBitsDirtyWordFlags.size() < cpuWordCount) {
        m_streamingNonResidentBitsDirtyWordFlags.resize(cpuWordCount, 0u);
    }

    if (!m_streamingNonResidentBitsDirtyWordsSorted) {
        ZoneScopedN("CLodStreamingSystem::TryConsumeNonResidentBitsUpload::CompactSortDirtyWords");
        size_t writeIndex = 0u;
        for (size_t readIndex = m_streamingNonResidentBitsDirtyWordCursor;
            readIndex < m_streamingNonResidentBitsDirtyWords.size();
            ++readIndex) {
            const uint32_t word = m_streamingNonResidentBitsDirtyWords[readIndex];
            if (word < cpuWordCount && m_streamingNonResidentBitsDirtyWordFlags[word] != 0u) {
                m_streamingNonResidentBitsDirtyWords[writeIndex++] = word;
            }
        }
        m_streamingNonResidentBitsDirtyWords.resize(writeIndex);
        m_streamingNonResidentBitsDirtyWordCursor = 0u;
        std::sort(m_streamingNonResidentBitsDirtyWords.begin(), m_streamingNonResidentBitsDirtyWords.end());
        m_streamingNonResidentBitsDirtyWords.erase(
            std::unique(m_streamingNonResidentBitsDirtyWords.begin(), m_streamingNonResidentBitsDirtyWords.end()),
            m_streamingNonResidentBitsDirtyWords.end());
        m_streamingNonResidentBitsDirtyWordsSorted = true;
    }

    size_t beginIndex = m_streamingNonResidentBitsDirtyWordCursor;
    while (beginIndex < m_streamingNonResidentBitsDirtyWords.size() &&
        (m_streamingNonResidentBitsDirtyWords[beginIndex] >= cpuWordCount ||
            m_streamingNonResidentBitsDirtyWordFlags[m_streamingNonResidentBitsDirtyWords[beginIndex]] == 0u)) {
        ++beginIndex;
    }
    m_streamingNonResidentBitsDirtyWordCursor = beginIndex;

    if (beginIndex >= m_streamingNonResidentBitsDirtyWords.size() ||
        m_streamingNonResidentBitsDirtyWords[beginIndex] >= validWordCount) {
        if (m_pendingStreamingGpuStorageGroupCapacity == 0u) {
            m_streamingNonResidentBitsUploadPending = false;
            m_streamingNonResidentBitsDirtyBegin = 0u;
            m_streamingNonResidentBitsDirtyEnd = 0u;
            for (uint32_t word : m_streamingNonResidentBitsDirtyWords) {
                if (word < m_streamingNonResidentBitsDirtyWordFlags.size()) {
                    m_streamingNonResidentBitsDirtyWordFlags[word] = 0u;
                }
            }
            m_streamingNonResidentBitsDirtyWords.clear();
            m_streamingNonResidentBitsDirtyWordCursor = 0u;
        }
        return false;
    }

    const uint32_t firstWord = m_streamingNonResidentBitsDirtyWords[beginIndex];
    uint32_t lastWordExclusive = firstWord + 1u;
    size_t endIndex = beginIndex + 1u;
    while (endIndex < m_streamingNonResidentBitsDirtyWords.size() &&
        lastWordExclusive < validWordCount &&
        lastWordExclusive - firstWord < maxWords &&
        m_streamingNonResidentBitsDirtyWords[endIndex] == lastWordExclusive) {
        ++lastWordExclusive;
        ++endIndex;
    }

    {
        ZoneScopedN("CLodStreamingSystem::TryConsumeNonResidentBitsUpload::CopyRun");
        outBits.assign(
            m_streamingNonResidentBitsCpu.begin() + firstWord,
            m_streamingNonResidentBitsCpu.begin() + lastWordExclusive);
    }
    outFirstWord = firstWord;

    for (size_t i = beginIndex; i < endIndex; ++i) {
        const uint32_t word = m_streamingNonResidentBitsDirtyWords[i];
        if (word < m_streamingNonResidentBitsDirtyWordFlags.size()) {
            m_streamingNonResidentBitsDirtyWordFlags[word] = 0u;
        }
    }
    m_streamingNonResidentBitsDirtyWordCursor = endIndex;

    size_t nextIndex = m_streamingNonResidentBitsDirtyWordCursor;
    while (nextIndex < m_streamingNonResidentBitsDirtyWords.size() &&
        (m_streamingNonResidentBitsDirtyWords[nextIndex] >= cpuWordCount ||
            m_streamingNonResidentBitsDirtyWordFlags[m_streamingNonResidentBitsDirtyWords[nextIndex]] == 0u)) {
        ++nextIndex;
    }
    m_streamingNonResidentBitsDirtyWordCursor = nextIndex;

    if (m_streamingNonResidentBitsDirtyWordCursor >= m_streamingNonResidentBitsDirtyWords.size()) {
        m_streamingNonResidentBitsDirtyWords.clear();
        m_streamingNonResidentBitsDirtyWordCursor = 0u;
        m_streamingNonResidentBitsDirtyBegin = 0u;
        m_streamingNonResidentBitsDirtyEnd = 0u;
        m_streamingNonResidentBitsUploadPending = false;
        RecordNonResidentBitsUploadQueued();
    }
    else {
        const uint32_t nextWord = m_streamingNonResidentBitsDirtyWords[m_streamingNonResidentBitsDirtyWordCursor];
        m_streamingNonResidentBitsDirtyBegin = nextWord;
        m_streamingNonResidentBitsDirtyEnd = std::min<uint32_t>(
            validWordCount,
            m_streamingNonResidentBitsDirtyWords.back() + 1u);
        m_streamingNonResidentBitsUploadPending = true;
    }

    TracyPlot("CLodStreaming.NonResidentBits.UploadWords", static_cast<int64_t>(outBits.size()));
    TracyPlot(
        "CLodStreaming.NonResidentBits.PendingWords",
        static_cast<int64_t>(m_streamingNonResidentBitsDirtyWords.size() - m_streamingNonResidentBitsDirtyWordCursor));
    return !outBits.empty();
}

void CLodStreamingSystem::MarkStreamingActiveGroupsBitsDirty() {
    m_streamingActiveGroupsBitsUploadPending = true;
}

uint32_t CLodStreamingSystem::UnpackStreamingRequestPriority(const CLodStreamingRequest& req) {
    return (req.viewId >> 16u) & 0xFFFFu;
}

bool CLodStreamingSystem::IsGroupPinned(uint32_t groupIndex) const {
    const uint32_t wordAddress = BitWordAddress(groupIndex);
    if (wordAddress >= m_streamingPinnedGroupsBitsCpu.size()) {
        return false;
    }

    return (m_streamingPinnedGroupsBitsCpu[wordAddress] & BitMask(groupIndex)) != 0u;
}

bool CLodStreamingSystem::IsGroupActive(uint32_t groupIndex) const {
    const uint32_t wordAddress = BitWordAddress(groupIndex);
    if (wordAddress >= m_streamingActiveGroupsBitsCpu.size()) {
        return false;
    }

    return (m_streamingActiveGroupsBitsCpu[wordAddress] & BitMask(groupIndex)) != 0u;
}

bool CLodStreamingSystem::IsGroupResident(uint32_t groupIndex) const {
    const uint32_t wordAddress = BitWordAddress(groupIndex);
    if (wordAddress >= m_streamingNonResidentBitsCpu.size()) {
        return false;
    }

    return (m_streamingNonResidentBitsCpu[wordAddress] & BitMask(groupIndex)) == 0u;
}

bool CLodStreamingSystem::SetGroupResidentBit(uint32_t groupIndex, bool resident) {
    const uint32_t wordAddress = BitWordAddress(groupIndex);
    if (wordAddress >= m_streamingNonResidentBitsCpu.size()) {
        return false;
    }

    const uint32_t bitMask = BitMask(groupIndex);
    const bool wasResident = (m_streamingNonResidentBitsCpu[wordAddress] & bitMask) == 0u;
    if (wasResident == resident) {
        return false;
    }

    ++m_streamingResidencyMutationEpoch;
    if (resident) {
        m_streamingNonResidentBitsCpu[wordAddress] &= ~bitMask;
        ++m_streamingResidentGroupsCount;
    } else {
        m_streamingNonResidentBitsCpu[wordAddress] |= bitMask;
        if (m_streamingResidentGroupsCount > 0u) {
            --m_streamingResidentGroupsCount;
        }
    }
    MarkStreamingNonResidentBitsDirtyWord(wordAddress);
    spdlog::debug(
        "CLod streaming invariant: residency bit cpu-change group={} resident={} epoch={} tick={} dirtyWords=[{}, {})",
        groupIndex,
        resident,
        m_streamingResidencyMutationEpoch,
        m_streamingDiagnosticTick,
        m_streamingNonResidentBitsDirtyBegin,
        m_streamingNonResidentBitsDirtyEnd);
    return true;
}

bool CLodStreamingSystem::UsesPinnedStorage(uint32_t groupIndex) const {
    return m_groupsUsingPinnedStorage.count(groupIndex) != 0u;
}

bool CLodStreamingSystem::TryQueuePendingLoadRequest(const CLodStreamingRequest& req, uint32_t priority) {
    const uint32_t groupIndex = req.groupGlobalIndex;
    if (groupIndex >= m_streamingStorageGroupCapacity) {
        EnsureStreamingStorageCapacity(groupIndex + 1u);
    }
    RecordStreamingRequestObserved(groupIndex, priority);

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

uint32_t CLodStreamingSystem::QueueLoadRequestWithParents(const CLodStreamingRequest& requestedLoad, uint32_t requestedPriority) {
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
        if (!TryGetCachedParentGroup(currentGroup, parentGroup)) {
            break;
        }
        if (parentGroup == currentGroup) {
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
        if (TryQueuePendingLoadRequest(parentLoad, parentPriority)) {
            queuedCount++;
        }
    }

    if (TryQueuePendingLoadRequest(requestedLoad, requestedPriority)) {
        queuedCount++;
    }

    return queuedCount;
}

void CLodStreamingSystem::InitializePageLru(MeshManager* meshManager) {
    ZoneScopedN("CLodStreamingSystem::InitializePageLru");

    if (m_pageLruInitialized || !meshManager) return;

    auto* pool = meshManager->GetCLodPagePool();
    if (!pool) return;

    const uint32_t generalPages = pool->GetGeneralPageCount();
    const uint32_t totalPages = pool->GetTotalPageCount();
    if (generalPages == 0 || totalPages == 0) return;

    m_pageOwnerGroup.assign(totalPages, -1);
    m_pageOwnerSegment.resize(totalPages, 0u);
    m_pageState.assign(totalPages, CLodPhysicalPageState::Free);
    m_pageRetireAfterTick.assign(totalPages, 0u);
    m_pageRetirePinned.assign(totalPages, 0u);
    m_pagePinnedStorage.assign(totalPages, 0u);
    m_pageReuseRequiresNonResidentEpoch.assign(totalPages, 0u);
    m_pageReuseNonResidentQueuedTick.assign(totalPages, 0u);
    m_pageReuseUploadFenceValue.assign(totalPages, 0u);
    m_retiringPhysicalPages.clear();
    m_retiringPagesAwaitingUploadFence.clear();
    m_pendingPageOwnerGroup.assign(totalPages, ~0u);
    m_pendingPageOwnerSegment.assign(totalPages, 0u);
    m_pageOwnerMeshPageKey.assign(totalPages, kInvalidCLodMeshPageKey);
    m_pageResidentGroups.clear();
    m_pageResidentGroups.resize(totalPages);
    m_pageProtectedThisUpdate.assign(totalPages, 0u);

    {
        ZoneScopedN("CLodStreamingSystem::InitializePageLru::PopulateFreePages");
        for (uint32_t p = 0; p < generalPages; ++p) {
            m_pageLru.Insert(p);
        }
    }

    m_pageLruInitialized = true;

    // Route PagePool uploads through the worker-owned CLod upload stream.
    InstallStreamingUploadFunction(meshManager);
    spdlog::info("CLodPageLRU initialized with {} general pages", generalPages);
}

void CLodStreamingSystem::EnsurePageTrackingCapacity(MeshManager* meshManager) {
    if (meshManager == nullptr) {
        return;
    }

    auto* pool = meshManager->GetCLodPagePool();
    if (pool == nullptr) {
        return;
    }

    const uint32_t totalPages = pool->GetTotalPageCount();
    if (m_pageOwnerGroup.size() < totalPages) {
        m_pageOwnerGroup.resize(totalPages, -1);
        m_pageOwnerSegment.resize(totalPages, 0u);
        m_pageState.resize(totalPages, CLodPhysicalPageState::Free);
        m_pageRetireAfterTick.resize(totalPages, 0u);
        m_pageRetirePinned.resize(totalPages, 0u);
        m_pagePinnedStorage.resize(totalPages, 0u);
        m_pageReuseRequiresNonResidentEpoch.resize(totalPages, 0u);
        m_pageReuseNonResidentQueuedTick.resize(totalPages, 0u);
        m_pageReuseUploadFenceValue.resize(totalPages, 0u);
        m_pendingPageOwnerGroup.resize(totalPages, ~0u);
        m_pendingPageOwnerSegment.resize(totalPages, 0u);
        m_pageOwnerMeshPageKey.resize(totalPages, kInvalidCLodMeshPageKey);
        m_pageResidentGroups.resize(totalPages);
        m_pageProtectedThisUpdate.resize(totalPages, 0u);
    }
}

void CLodStreamingSystem::ReleaseOwnedPagesForGroup(uint32_t groupIndex, MeshManager* meshManager) {
    ReleaseGroupResidency(groupIndex, meshManager, false);
}

uint64_t CLodStreamingSystem::StreamingUploadVisibilityDelayTicks() const {
    const uint32_t framesInFlight = static_cast<uint32_t>(std::max<uint8_t>(
        rg::runtime::GetOpenRenderGraphSettings().numFramesInFlight,
        uint8_t{1}));
    return static_cast<uint64_t>(std::max<uint32_t>(m_streamingReadbackRingSize, framesInFlight) + 2u);
}

void CLodStreamingSystem::RecordNonResidentBitsUploadQueued() {
    m_streamingNonResidentBitsQueuedEpoch = m_streamingResidencyMutationEpoch;
    m_streamingNonResidentBitsQueuedTick = m_streamingDiagnosticTick;
    spdlog::debug(
        "CLod streaming invariant: queued nonresident-bit upload epoch={} tick={}",
        m_streamingNonResidentBitsQueuedEpoch,
        m_streamingNonResidentBitsQueuedTick);
}

void CLodStreamingSystem::QueuePendingNonResidentBitsUpload() {
    ZoneScopedN("CLodStreamingWorker::QueueNonResidentBitsUpload");
    if (m_uploadStream == nullptr || !m_streamingNonResidentBitsUploadPending) {
        return;
    }

    constexpr uint32_t kWorkerMaxNonResidentUploadWordsPerRun = 16384u;
    constexpr uint32_t kWorkerMaxNonResidentUploadRuns = 64u;
    std::vector<uint32_t> uploadBits;
    uint32_t firstWord = 0u;
    uint32_t uploadedRuns = 0u;
    uint64_t uploadedWords = 0u;
    while (uploadedRuns < kWorkerMaxNonResidentUploadRuns &&
        TryConsumeStreamingNonResidentBitsUpload(uploadBits, firstWord, kWorkerMaxNonResidentUploadWordsPerRun)) {
        m_uploadStream->UploadData(
            uploadBits.data(),
            uploadBits.size() * sizeof(uint32_t),
            rg::runtime::UploadTarget::FromShared(m_streamingNonResidentBits),
            firstWord * sizeof(uint32_t));
        uploadedWords += static_cast<uint64_t>(uploadBits.size());
        ++uploadedRuns;
    }

    TracyPlot("CLodStreaming.NonResidentBits.WorkerUploadRuns", static_cast<int64_t>(uploadedRuns));
    TracyPlot("CLodStreaming.NonResidentBits.WorkerUploadWords", static_cast<int64_t>(uploadedWords));
}

void CLodStreamingSystem::LogPageOverwriteInvariant(
    uint32_t page,
    uint32_t newGroupIndex,
    uint32_t segmentIndex,
    uint64_t meshPageKey,
    const char* reason) const {
    if (page == ~0u || page >= m_pageState.size()) {
        return;
    }

    const uint64_t requiredEpoch = page < m_pageReuseRequiresNonResidentEpoch.size()
        ? m_pageReuseRequiresNonResidentEpoch[page]
        : 0u;
    const uint64_t queuedTick = page < m_pageReuseNonResidentQueuedTick.size()
        ? m_pageReuseNonResidentQueuedTick[page]
        : 0u;
    const uint64_t delayTicks = StreamingUploadVisibilityDelayTicks();
    const bool nonResidentUploadNotQueued =
        requiredEpoch != 0u && requiredEpoch > m_streamingNonResidentBitsQueuedEpoch;
    const bool pageMayStillBeVisibleToTraversal =
        queuedTick != 0u && m_streamingDiagnosticTick <= queuedTick + delayTicks;
    const bool hasResidentGroups =
        page < m_pageResidentGroups.size() && !m_pageResidentGroups[page].empty();
    const bool pendingOwnerConflict =
        page < m_pendingPageOwnerGroup.size() &&
        m_pendingPageOwnerGroup[page] != ~0u &&
        m_pendingPageOwnerGroup[page] != newGroupIndex;

    if (!nonResidentUploadNotQueued &&
        !pageMayStillBeVisibleToTraversal &&
        !hasResidentGroups &&
        !pendingOwnerConflict) {
        return;
    }

    spdlog::debug(
        "CLod streaming invariant violation? overwriting page before old users are provably hidden: reason={} newGroup={} seg={} page={} key={} state={} ownerGroup={} residentGroups={} pendingOwner={} requiredNonResidentEpoch={} queuedNonResidentEpoch={} retireQueuedTick={} currentTick={} visibilityDelay={}",
        reason != nullptr ? reason : "unknown",
        newGroupIndex,
        segmentIndex,
        page,
        meshPageKey,
        static_cast<uint32_t>(m_pageState[page]),
        page < m_pageOwnerGroup.size() ? m_pageOwnerGroup[page] : -1,
        page < m_pageResidentGroups.size() ? static_cast<uint32_t>(m_pageResidentGroups[page].size()) : 0u,
        page < m_pendingPageOwnerGroup.size() ? m_pendingPageOwnerGroup[page] : UINT32_MAX,
        requiredEpoch,
        m_streamingNonResidentBitsQueuedEpoch,
        queuedTick,
        m_streamingDiagnosticTick,
        delayTicks);
}

void CLodStreamingSystem::PrefetchChildGroupLayouts(uint32_t parentGroupIndex, MeshManager* meshManager) {
    if (meshManager == nullptr) {
        return;
    }

    m_childGroupsScratch.clear();
    meshManager->GetCLodChildGroups(parentGroupIndex, m_childGroupsScratch);
    if (m_childGroupsScratch.empty()) {
        return;
    }

    EvictPrefetchedChildLayoutsForOwner(parentGroupIndex);

    std::vector<uint32_t> insertedChildren;
    insertedChildren.reserve(m_childGroupsScratch.size());

    for (uint32_t childGroupIndex : m_childGroupsScratch) {
        CLodCache::GroupPayloadLayoutMetadata layout;
        std::string message;
        if (!meshManager->TryGetCLodGroupPayloadLayout(childGroupIndex, layout, &message) || !layout.IsValid()) {
            spdlog::debug(
                "CLod streaming: child header prefetch miss for parent {} child {}: {}",
                parentGroupIndex,
                childGroupIndex,
                message.empty() ? "layout unavailable" : message);
            continue;
        }

        CachedChildGroupLayout cachedLayout{};
        cachedLayout.ownerGroupIndex = parentGroupIndex;
        cachedLayout.layout = std::move(layout);
        m_prefetchedChildLayoutsByGroup[childGroupIndex] = std::move(cachedLayout);
        insertedChildren.push_back(childGroupIndex);
    }

    if (!insertedChildren.empty()) {
        m_prefetchedChildLayoutKeysByOwner[parentGroupIndex] = std::move(insertedChildren);
    }
}

void CLodStreamingSystem::InstallPrefetchedChildGroupLayouts(
    uint32_t parentGroupIndex,
    std::vector<MeshManager::CLodPrefetchedChildLayout>&& prefetchedLayouts) {
    if (prefetchedLayouts.empty()) {
        return;
    }

    EvictPrefetchedChildLayoutsForOwner(parentGroupIndex);

    std::vector<uint32_t> insertedChildren;
    insertedChildren.reserve(prefetchedLayouts.size());

    for (auto& prefetchedLayout : prefetchedLayouts) {
        if (!prefetchedLayout.layout.IsValid()) {
            continue;
        }

        const uint32_t childGroupIndex = prefetchedLayout.groupGlobalIndex;
        CachedChildGroupLayout cachedLayout{};
        cachedLayout.ownerGroupIndex = parentGroupIndex;
        cachedLayout.layout = std::move(prefetchedLayout.layout);
        m_prefetchedChildLayoutsByGroup[childGroupIndex] = std::move(cachedLayout);
        insertedChildren.push_back(childGroupIndex);
    }

    if (!insertedChildren.empty()) {
        m_prefetchedChildLayoutKeysByOwner[parentGroupIndex] = std::move(insertedChildren);
    }
}

void CLodStreamingSystem::EvictPrefetchedChildLayoutsForOwner(uint32_t ownerGroupIndex) {
    auto ownerIt = m_prefetchedChildLayoutKeysByOwner.find(ownerGroupIndex);
    if (ownerIt == m_prefetchedChildLayoutKeysByOwner.end()) {
        return;
    }

    for (uint32_t childGroupIndex : ownerIt->second) {
        auto layoutIt = m_prefetchedChildLayoutsByGroup.find(childGroupIndex);
        if (layoutIt != m_prefetchedChildLayoutsByGroup.end() && layoutIt->second.ownerGroupIndex == ownerGroupIndex) {
            m_prefetchedChildLayoutsByGroup.erase(layoutIt);
        }
    }

    m_prefetchedChildLayoutKeysByOwner.erase(ownerIt);
}

void CLodStreamingSystem::ClearPrefetchedChildLayouts() {
    m_prefetchedChildLayoutsByGroup.clear();
    m_prefetchedChildLayoutKeysByOwner.clear();
}

void CLodStreamingSystem::EnsureStreamingDiagnosticsCapacity(uint32_t requiredGroupCount) {
    if (requiredGroupCount > m_streamingDiagnosticsByGroup.size()) {
        m_streamingDiagnosticsByGroup.resize(requiredGroupCount);
    }
}

void CLodStreamingSystem::RecordStreamingRequestObserved(uint32_t groupIndex, uint32_t priority) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag = {};
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    diag.priority = std::max(diag.priority, priority);
}

void CLodStreamingSystem::RecordStreamingRequestQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.cpuQueuedTick == 0u) {
        diag.cpuQueuedTick = m_streamingDiagnosticTick;
    }
}

void CLodStreamingSystem::RecordStreamingDuplicateRequest(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    ++diag.duplicateRequests;
    ++m_streamingDiagnosticsDuplicateRequestsThisFrame;
}

void CLodStreamingSystem::RecordStreamingDiskQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.diskQueuedTick == 0u) {
        diag.diskQueuedTick = m_streamingDiagnosticTick;
    }
}

void CLodStreamingSystem::RecordStreamingCompletion(
    uint32_t groupIndex,
    const MeshManager::CLodDiskStreamingCompletion& completion) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    diag.diskCompletedTick = m_streamingDiagnosticTick;
    diag.uploadedBytes = completion.totalStreamedBytes;
    if (completion.success) {
        ++m_streamingDiagnosticsCompletionSuccessThisFrame;
    } else {
        ++m_streamingDiagnosticsCompletionFailedThisFrame;
    }
    if (diag.diskQueuedTick != 0u) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(m_streamingDiagnosticTick - diag.diskQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame;
        m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame += ticks;
        m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame =
            std::max(m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame, ticks);
    }
}

void CLodStreamingSystem::RecordStreamingUploadQueued(uint32_t groupIndex, uint64_t bytes) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.uploadQueuedTick == 0u) {
        diag.uploadQueuedTick = m_streamingDiagnosticTick;
        ++m_streamingDiagnosticsUploadQueuedGroupsThisFrame;
        if (diag.active) {
            const uint32_t ticks = static_cast<uint32_t>(
                std::min<uint64_t>(m_streamingDiagnosticTick - diag.firstRequestTick, UINT32_MAX));
            ++m_streamingDiagnosticsRequestToUploadSamplesThisFrame;
            m_streamingDiagnosticsRequestToUploadSumThisFrame += ticks;
            if (ticks > m_streamingDiagnosticsRequestToUploadWorstThisFrame) {
                m_streamingDiagnosticsRequestToUploadWorstThisFrame = ticks;
                m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame = groupIndex;
            }
        }
    }
    m_streamingDiagnosticsUploadQueuedBytesThisFrame += bytes;
}

void CLodStreamingSystem::RecordStreamingCommitQueued(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        diag.firstRequestTick = m_streamingDiagnosticTick;
        diag.active = true;
    }
    if (diag.commitQueuedTick == 0u) {
        diag.commitQueuedTick = m_streamingDiagnosticTick;
    }
}

void CLodStreamingSystem::RecordStreamingPromoted(uint32_t groupIndex) {
    EnsureStreamingDiagnosticsCapacity(groupIndex + 1u);
    auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
    if (!diag.active) {
        return;
    }

    diag.residentTick = m_streamingDiagnosticTick;
    const uint32_t requestToResident = static_cast<uint32_t>(
        std::min<uint64_t>(diag.residentTick - diag.firstRequestTick, UINT32_MAX));
    ++m_streamingDiagnosticsRequestToResidentSamplesThisFrame;
    m_streamingDiagnosticsRequestToResidentSumThisFrame += requestToResident;
    if (requestToResident > m_streamingDiagnosticsRequestToResidentWorstThisFrame) {
        m_streamingDiagnosticsRequestToResidentWorstThisFrame = requestToResident;
        m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame = groupIndex;
    }
    if (diag.uploadQueuedTick != 0u) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(diag.residentTick - diag.uploadQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsUploadToResidentSamplesThisFrame;
        m_streamingDiagnosticsUploadToResidentSumThisFrame += ticks;
        m_streamingDiagnosticsUploadToResidentWorstThisFrame =
            std::max(m_streamingDiagnosticsUploadToResidentWorstThisFrame, ticks);
    }
    if (diag.commitQueuedTick != 0u) {
        const uint32_t ticks = static_cast<uint32_t>(
            std::min<uint64_t>(diag.residentTick - diag.commitQueuedTick, UINT32_MAX));
        ++m_streamingDiagnosticsCommitToResidentSamplesThisFrame;
        m_streamingDiagnosticsCommitToResidentSumThisFrame += ticks;
        m_streamingDiagnosticsCommitToResidentWorstThisFrame =
            std::max(m_streamingDiagnosticsCommitToResidentWorstThisFrame, ticks);
    }

    diag = {};
}

void CLodStreamingSystem::RecordStreamingTerminal(uint32_t groupIndex) {
    if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
        m_streamingDiagnosticsByGroup[groupIndex] = {};
    }
}

void CLodStreamingSystem::AccumulateStreamingDiagnostics(CLodStreamingOperationStats& stats) {
    stats.duplicateRequests = m_streamingDiagnosticsDuplicateRequestsThisFrame;
    stats.preallocationDeferrals = m_streamingDiagnosticsPreallocationDeferralsThisFrame;
    stats.promotionDeferrals = m_streamingDiagnosticsPromotionDeferralsThisFrame;
    stats.completionSuccess = m_streamingDiagnosticsCompletionSuccessThisFrame;
    stats.completionFailed = m_streamingDiagnosticsCompletionFailedThisFrame;
    stats.uploadQueuedGroups = m_streamingDiagnosticsUploadQueuedGroupsThisFrame;
    stats.uploadQueuedBytes = m_streamingDiagnosticsUploadQueuedBytesThisFrame;
    stats.requestToUploadSamples = m_streamingDiagnosticsRequestToUploadSamplesThisFrame;
    stats.requestToUploadAvgTicks = stats.requestToUploadSamples != 0u
        ? static_cast<uint32_t>(m_streamingDiagnosticsRequestToUploadSumThisFrame / stats.requestToUploadSamples)
        : 0u;
    stats.requestToUploadWorstTicks = m_streamingDiagnosticsRequestToUploadWorstThisFrame;
    stats.requestToUploadWorstGroup = m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame;
    stats.requestToResidentSamples = m_streamingDiagnosticsRequestToResidentSamplesThisFrame;
    stats.requestToResidentAvgTicks = stats.requestToResidentSamples != 0u
        ? static_cast<uint32_t>(m_streamingDiagnosticsRequestToResidentSumThisFrame / stats.requestToResidentSamples)
        : 0u;
    stats.requestToResidentWorstTicks = m_streamingDiagnosticsRequestToResidentWorstThisFrame;
    stats.requestToResidentWorstGroup = m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame;
    stats.diskQueueToCompleteAvgTicks = m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame != 0u
        ? static_cast<uint32_t>(m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame / m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame)
        : 0u;
    stats.diskQueueToCompleteWorstTicks = m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame;
    stats.uploadToResidentAvgTicks = m_streamingDiagnosticsUploadToResidentSamplesThisFrame != 0u
        ? static_cast<uint32_t>(m_streamingDiagnosticsUploadToResidentSumThisFrame / m_streamingDiagnosticsUploadToResidentSamplesThisFrame)
        : 0u;
    stats.uploadToResidentWorstTicks = m_streamingDiagnosticsUploadToResidentWorstThisFrame;
    stats.commitToResidentAvgTicks = m_streamingDiagnosticsCommitToResidentSamplesThisFrame != 0u
        ? static_cast<uint32_t>(m_streamingDiagnosticsCommitToResidentSumThisFrame / m_streamingDiagnosticsCommitToResidentSamplesThisFrame)
        : 0u;
    stats.commitToResidentWorstTicks = m_streamingDiagnosticsCommitToResidentWorstThisFrame;

    uint32_t diskIoCount = 0u;
    for (uint32_t groupIndex = 0; groupIndex < static_cast<uint32_t>(m_streamingDiagnosticsByGroup.size()); ++groupIndex) {
        const auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
        if (!diag.active) {
            continue;
        }
        if (diag.cpuQueuedTick != 0u && diag.diskQueuedTick == 0u) {
            const uint32_t age = static_cast<uint32_t>(
                std::min<uint64_t>(m_streamingDiagnosticTick - diag.cpuQueuedTick, UINT32_MAX));
            if (age > stats.pendingCpuMaxAgeTicks) {
                stats.pendingCpuMaxAgeTicks = age;
                stats.pendingCpuMaxAgeGroup = groupIndex;
            }
        }
        if (diag.diskQueuedTick != 0u && diag.diskCompletedTick == 0u) {
            ++diskIoCount;
            const uint32_t age = static_cast<uint32_t>(
                std::min<uint64_t>(m_streamingDiagnosticTick - diag.diskQueuedTick, UINT32_MAX));
            if (age > stats.diskIoMaxAgeTicks) {
                stats.diskIoMaxAgeTicks = age;
                stats.diskIoMaxAgeGroup = groupIndex;
            }
        }
        if (diag.commitQueuedTick != 0u) {
            const uint32_t age = static_cast<uint32_t>(
                std::min<uint64_t>(m_streamingDiagnosticTick - diag.commitQueuedTick, UINT32_MAX));
            if (age > stats.pendingCommitMaxAgeTicks) {
                stats.pendingCommitMaxAgeTicks = age;
                stats.pendingCommitMaxAgeGroup = groupIndex;
            }
        }
    }
    stats.pendingCpuRequests = m_pendingStreamingRequestCount;
    stats.pendingCpuHeapRequests = static_cast<uint32_t>(m_pendingStreamingRequests.size());
    stats.waitingForPagesRequests = m_waitingForPagesRequestCount;
    stats.inProgressRequests = m_streamingRequestsInProgressCount;
    stats.diskIoRequests = diskIoCount;
    stats.pendingCommitGroups = static_cast<uint32_t>(m_pendingResidencyCommitGroups.size());
    stats.readyCompletions = static_cast<uint32_t>(m_readyStreamingCompletionsByGroup.size());

    for (const PendingStreamingRequest& pending : m_pendingStreamingRequests) {
        const uint32_t groupIndex = pending.request.groupGlobalIndex;
        if (groupIndex >= m_streamingDiagnosticsByGroup.size()) {
            continue;
        }
        const auto& diag = m_streamingDiagnosticsByGroup[groupIndex];
        if (!diag.active) {
            continue;
        }
        const uint64_t queuedTick = diag.cpuQueuedTick != 0u ? diag.cpuQueuedTick : diag.firstRequestTick;
        const uint32_t age = static_cast<uint32_t>(
            std::min<uint64_t>(m_streamingDiagnosticTick - queuedTick, UINT32_MAX));
        if (age > stats.pendingCpuMaxAgeTicks) {
            stats.pendingCpuMaxAgeTicks = age;
            stats.pendingCpuMaxAgeGroup = groupIndex;
        }
    }

    constexpr uint32_t kOutlierTicks = 180u;
    const bool hasOutlier =
        stats.requestToResidentWorstTicks >= kOutlierTicks ||
        stats.pendingCpuMaxAgeTicks >= kOutlierTicks ||
        stats.diskIoMaxAgeTicks >= kOutlierTicks ||
        stats.pendingCommitMaxAgeTicks >= kOutlierTicks;
    if (hasOutlier &&
        (m_streamingDiagnosticsLastOutlierLogTick == 0u ||
            m_streamingDiagnosticTick >= m_streamingDiagnosticsLastOutlierLogTick + 120u)) {
        m_streamingDiagnosticsLastOutlierLogTick = m_streamingDiagnosticTick;
        spdlog::debug(
            "CLod streaming latency diag[tick={}]: req->resident worst={} group={} samples={} req->upload worst={} group={} pendingCpuMax={} group={} diskIoMax={} group={} commitMax={} group={} pendingCpu={} inProgress={} diskIo={} pendingCommit={} readyCompletions={} preallocDeferrals={} promotionDeferrals={}",
            m_streamingDiagnosticTick,
            stats.requestToResidentWorstTicks,
            stats.requestToResidentWorstGroup,
            stats.requestToResidentSamples,
            stats.requestToUploadWorstTicks,
            stats.requestToUploadWorstGroup,
            stats.pendingCpuMaxAgeTicks,
            stats.pendingCpuMaxAgeGroup,
            stats.diskIoMaxAgeTicks,
            stats.diskIoMaxAgeGroup,
            stats.pendingCommitMaxAgeTicks,
            stats.pendingCommitMaxAgeGroup,
            stats.pendingCpuRequests,
            stats.inProgressRequests,
            stats.diskIoRequests,
            stats.pendingCommitGroups,
            stats.readyCompletions,
            stats.preallocationDeferrals,
            stats.promotionDeferrals);
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

void CLodStreamingSystem::RetirePhysicalPage(uint32_t page, MeshManager* meshManager, bool pinned) {
    if (page == ~0u || page >= m_pageState.size()) {
        return;
    }

    if (m_pageState[page] == CLodPhysicalPageState::Retiring) {
        if (page < m_pageRetireAfterTick.size()) {
            const uint32_t framesInFlight = static_cast<uint32_t>(std::max<uint8_t>(
                rg::runtime::GetOpenRenderGraphSettings().numFramesInFlight,
                uint8_t{1}));
            const uint64_t retireDelayTicks = static_cast<uint64_t>(
                std::max<uint32_t>(m_streamingReadbackRingSize, framesInFlight) + 2u);
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
        const uint32_t framesInFlight = static_cast<uint32_t>(std::max<uint8_t>(
            rg::runtime::GetOpenRenderGraphSettings().numFramesInFlight,
            uint8_t{1}));
        const uint64_t retireDelayTicks = static_cast<uint64_t>(
            std::max<uint32_t>(m_streamingReadbackRingSize, framesInFlight) + 2u);
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
    m_pageLru.Remove(page);
    (void)meshManager;
}

void CLodStreamingSystem::DrainRetiredPhysicalPages(MeshManager* meshManager) {
    ZoneScopedN("CLodStreamingSystem::DrainRetiredPhysicalPages");
    if (m_pageState.empty() || m_retiringPhysicalPages.empty()) {
        return;
    }

    TracyPlot("CLodStreaming.RetiringPhysicalPages", static_cast<int64_t>(m_retiringPhysicalPages.size()));
    std::vector<uint32_t> pinnedPagesToFree;
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
            m_pageLru.Insert(page);
        }
    }
    m_retiringPhysicalPages.resize(pendingWriteIndex);

    if (!pinnedPagesToFree.empty() && meshManager != nullptr) {
        if (PagePool* pool = meshManager->GetCLodPagePool()) {
            pool->FreePinnedPages(pinnedPagesToFree);
        }
    }
}

void CLodStreamingSystem::ReleaseGroupResidency(uint32_t groupIndex, MeshManager* meshManager, bool clearPageMapEntries) {
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
        if (MeshManager* meshManager = m_getMeshManager ? m_getMeshManager() : nullptr) {
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
                m_pageLru.Touch(page);
            }
        }
        return true;
    };

    // Every group is protected through this function, so an already-protected
    // node implies that its ancestor chain was handled earlier this update.
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

bool CLodStreamingSystem::EvictPhysicalPage(uint32_t page, MeshManager* meshManager) {
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
        SetGroupResidentBit(groupIndex, false);
        ReleaseGroupResidency(groupIndex, meshManager, false);
    }

    if (page < m_pageState.size() && m_pageState[page] != CLodPhysicalPageState::Retiring) {
        RetirePhysicalPage(page, meshManager, IsPhysicalPagePinnedStorage(page));
    }
    return true;
}

std::vector<uint32_t> CLodStreamingSystem::PopFreePages(uint32_t count, MeshManager* meshManager) {
    return PopFreePages(count, meshManager, nullptr);
}

std::vector<uint32_t> CLodStreamingSystem::PopFreePages(uint32_t count, MeshManager* meshManager, PagePopFailureStats* outStats) {
    ZoneScopedN("CLodStreamingSystem::PopFreePages");
    ZoneValue(count);

    std::vector<uint32_t> pages;
    pages.reserve(count);

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

        m_pageLru.Remove(page);
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

    const uint32_t lruSize = m_pageLru.Size();
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
            uint32_t page = m_pageLru.PopOldest();
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
        ZoneScopedN("CLodStreamingSystem::PopFreePages::EvictResidentPages");
        uint32_t attemptsRemaining = scanLimit;
        while (pages.size() < count && attemptsRemaining-- > 0u) {
            uint32_t page = m_pageLru.PopOldest();
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
    uint32_t groupIndex, const MeshManager::CLodGroupStreamingInfo& info, MeshManager* meshManager) {
    ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup");

    const uint32_t segmentCount = info.valid ? info.pageCount : 1u;
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

    if (info.valid && info.meshPageIndices.size() == segmentCount) {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::BuildMeshPageKeys");
        for (uint32_t seg = 0; seg < segmentCount; ++seg) {
            result.meshPageKeys[seg] = MakeCLodMeshPageKey(info.groupsBase, info.meshPageIndices[seg]);
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
                    m_pageLru.Touch(existingPage);
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
                    m_pageLru.Touch(existingPage);
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
    if (missingCount == 0u) {
        freshPages.clear();
    } else if (result.usesPinnedStorage) {
        ZoneScopedN("CLodStreamingSystem::PreAllocatePagesForGroup::AllocatePinnedPages");
        ZoneValue(missingCount);
        auto* pool = meshManager ? meshManager->GetCLodPagePool() : nullptr;
        if (pool == nullptr) {
            return PreAllocatedPages{};
        }

        freshPages = pool->AllocatePinnedPages(missingCount);
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
        freshPages = PopFreePages(missingCount, meshManager, &popStats);
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
                        m_pageLru.Size(),
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
                        m_pageLru.Insert(page);
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

bool CLodStreamingSystem::AssignPagesToGroup(uint32_t groupIndex, const PreAllocatedPages& pages, MeshManager* meshManager) {
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
                m_pageLru.Remove(page);
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
                m_pageLru.Remove(page);
            }
            if (!IsPhysicalPagePinnedStorage(page) && !fetchedPage) {
                if (IsPhysicalPageResidentForKey(page, meshPageKey)) {
                    m_pageLru.Insert(page);
                }
            }
        }
    }
    return true;
}

void CLodStreamingSystem::ReleasePreAllocatedPages(const PreAllocatedPages& pages, MeshManager* meshManager) {
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
                    m_pageLru.Insert(page);
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
    const MeshManager::CLodDiskStreamingCompletion& completion,
    uint32_t expectedPageCount) const {
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

#if 0
        // Temporary CPU payload source-group validation disabled after page-lifecycle fix.
        const auto info = MeshManager::CLodGroupStreamingInfo{};
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
#endif
    }

    return true;
}

void CLodStreamingSystem::CommitPendingResidencyPromotions() {
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

    {
        ZoneScopedN("CLodStreamingSystem::CommitPendingResidencyPromotions::ApplyPromotions");
        for (uint32_t groupIndex : groups) {
            if (groupIndex >= m_streamingStorageGroupCapacity || !IsGroupActive(groupIndex)) {
                m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
                continue;
            }
            if (m_groupOwnedPages.find(groupIndex) == m_groupOwnedPages.end()) {
                m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
                continue;
            }
            const auto resolvedUploadFenceIt = m_pendingResidencyUploadFenceByGroup.find(groupIndex);
            const bool hasUploadFence = resolvedUploadFenceIt != m_pendingResidencyUploadFenceByGroup.end();
            const uint64_t completedUploadFence = m_streamingUploadCompletionFenceHandle.IsValid()
                ? m_streamingUploadCompletionFenceHandle.GetCompletedValue()
                : UINT64_MAX;
            const bool uploadBatchReady = hasUploadFence &&
                completedUploadFence >= resolvedUploadFenceIt->second;
            bool pagesReady = false;
            if (uploadBatchReady) {
                pagesReady = PromoteGroupPagesAfterUploadDrain(groupIndex);
            }
            if (!uploadBatchReady || !pagesReady) {
                if (!hasUploadFence) {
                    const bool awaitingSeal = std::find(
                        m_residencyGroupsAwaitingUploadFence.begin(),
                        m_residencyGroupsAwaitingUploadFence.end(),
                        groupIndex) != m_residencyGroupsAwaitingUploadFence.end();
                    const bool hasBatchTicket = std::any_of(
                        m_outstandingUploadBatches.begin(),
                        m_outstandingUploadBatches.end(),
                        [groupIndex](const auto& batch) {
                            return batch && batch->ticket &&
                                std::find(batch->affectedGroups.begin(), batch->affectedGroups.end(), groupIndex) !=
                                    batch->affectedGroups.end();
                        });
                    if (!awaitingSeal && !hasBatchTicket) {
                        spdlog::critical(
                            "CLod streaming invariant: pending-commit group {} has no published or retained upload batch",
                            groupIndex);
#if BUILD_TYPE == BUILD_TYPE_DEBUG
                        __debugbreak();
#endif
                    }
                }
                m_pendingResidencyCommitGroups.insert(groupIndex);
                if (groupIndex < m_streamingDiagnosticsByGroup.size()) {
                    ++m_streamingDiagnosticsByGroup[groupIndex].promotionDeferrals;
                }
                ++m_streamingDiagnosticsPromotionDeferralsThisFrame;
                continue;
            }

            SetGroupResidentBit(groupIndex, true);
            m_pendingResidencyUploadFenceByGroup.erase(groupIndex);
            RecordStreamingPromoted(groupIndex);
            ClearStreamingRequestInProgress(groupIndex);
            ClearPendingLoadPriority(groupIndex);
        }
    }
}

void CLodStreamingSystem::ReconcileStaleDiskIoRequests(MeshManager* meshManager) {
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
    const auto pagesIt = m_groupOwnedPages.find(groupIndex);
    if (pagesIt == m_groupOwnedPages.end()) {
        return true;
    }

    bool waitingForSharedPendingPage = false;
    for (uint32_t seg = 0; seg < static_cast<uint32_t>(pagesIt->second.size()); ++seg) {
        const uint32_t page = pagesIt->second[seg];
        if (page == ~0u || page >= m_pageState.size()) {
            continue;
        }

        const auto keysIt = m_groupOwnedMeshPageKeys.find(groupIndex);
        const uint64_t key = keysIt != m_groupOwnedMeshPageKeys.end() && seg < static_cast<uint32_t>(keysIt->second.size())
            ? keysIt->second[seg]
            : kInvalidCLodMeshPageKey;

        if (IsPhysicalPageResidentForKey(page, key)) {
            bool insertedGroup = false;
            if (page < m_pageResidentGroups.size()) {
                insertedGroup = m_pageResidentGroups[page].insert(groupIndex).second;
            }
            if (insertedGroup && key != kInvalidCLodMeshPageKey) {
                m_residentMeshPageRefCounts[key]++;
            }
            ReleasePendingMeshPageReference(page, key);
            if (!IsPhysicalPagePinnedStorage(page)) {
                m_pageLru.Insert(page);
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
            m_pageLru.Insert(page);
        }
    }

    return !waitingForSharedPendingPage;
}

void CLodStreamingSystem::ForceGroupNonResident(uint32_t groupIndex, MeshManager* meshManager, bool clearPageMapEntries) {
    SetGroupResidentBit(groupIndex, false);
    ReleaseGroupResidency(groupIndex, meshManager, clearPageMapEntries);
    m_pendingResidencyCommitGroups.erase(groupIndex);
}

void CLodStreamingSystem::TouchGroupPages(uint32_t groupIndex) {
    ZoneScopedN("CLodStreamingSystem::TouchGroupPages");

    auto it = m_groupOwnedPages.find(groupIndex);
    if (it != m_groupOwnedPages.end()) {
        for (uint32_t page : it->second) {
            if (page != ~0u) {
                m_pageLru.Touch(page);
            }
        }
    }

    uint32_t current = groupIndex;
    for (size_t hop = 0; hop < m_streamingStorageGroupCapacity; ++hop) {
        uint32_t parent = 0;
        if (!TryGetCachedParentGroup(current, parent) || parent == current) break;

        auto pit = m_groupOwnedPages.find(parent);
        if (pit != m_groupOwnedPages.end()) {
            for (uint32_t page : pit->second) {
                if (page != ~0u) {
                    m_pageLru.Touch(page);
                }
            }
        }
        current = parent;
    }
}

void CLodStreamingSystem::EnsureStreamingStorageCapacity(uint32_t requiredGroupCount) {
    if (requiredGroupCount <= m_streamingStorageGroupCapacity) {
        return;
    }

    const uint32_t newCapacity = CLodRoundUpCapacity(requiredGroupCount);
    const uint32_t newWordCount = CLodBitsetWordCount(newCapacity);

    RequestStreamingStorageGpuResize(newCapacity);

    m_streamingNonResidentBitsCpu.resize(newWordCount, ~0u);
    m_streamingNonResidentBitsDirtyWordFlags.resize(newWordCount, 0u);
    m_streamingActiveGroupsBitsCpu.resize(newWordCount, 0u);
    m_streamingPinnedGroupsBitsCpu.resize(newWordCount, 0u);
    m_streamingResidencyInitializedBitsCpu.resize(newWordCount, 0u);
    m_usedGroupsBitsCpu.resize(newWordCount, 0u);
    m_parentGroupByGroup.resize(newCapacity, UINT32_MAX);
    m_groupLastUsedTick.resize(newCapacity, 0u);
    m_streamingRequestStateByGroup.resize(newCapacity, StreamingRequestState::None);
    m_pendingLoadPriorityByGroup.resize(newCapacity, 0u);
    m_pendingStreamingRequestHeapIndexByGroup.resize(newCapacity, UINT32_MAX);
    m_pendingStreamingRequestGenerationByGroup.resize(newCapacity, 0u);
    m_waitingForPagesRequestIndexByGroup.resize(newCapacity, UINT32_MAX);
    EnsureStreamingDiagnosticsCapacity(newCapacity);
    m_streamingStorageGroupCapacity = newCapacity;

    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();
}

void CLodStreamingSystem::RequestStreamingStorageGpuResize(uint32_t newCapacity) {
    uint32_t pending = m_pendingStreamingGpuStorageGroupCapacity.load(std::memory_order_relaxed);
    while (pending < newCapacity &&
        !m_pendingStreamingGpuStorageGroupCapacity.compare_exchange_weak(
            pending, newCapacity, std::memory_order_release, std::memory_order_relaxed)) {
    }
    TracyPlot("CLodStreaming.Storage.PendingGpuCapacity", static_cast<int64_t>(
        m_pendingStreamingGpuStorageGroupCapacity.load(std::memory_order_relaxed)));
}

bool CLodStreamingSystem::PublishPendingStreamingStorageGpuResizeLocked() {
    const uint32_t oldCapacity = m_streamingGpuStorageGroupCapacity.load(std::memory_order_acquire);
    const uint32_t newCapacity = m_pendingStreamingGpuStorageGroupCapacity.load(std::memory_order_acquire);
    if (newCapacity == 0u || newCapacity <= oldCapacity) {
        return false;
    }

    if (!BufferBase::IsBackingMutationAllowedOnThisThread()) {
        TracyPlot("CLodStreaming.Storage.SkippedGpuResizeOutsideMutationScope", static_cast<int64_t>(1));
        return false;
    }

    const uint32_t newWordCount = CLodBitsetWordCount(newCapacity);
    spdlog::info(
        "CLod streaming: publishing deferred bitset GPU resize oldCapacity={} newCapacity={} words={}",
        oldCapacity,
        newCapacity,
        newWordCount);

    m_streamingNonResidentBits->ResizeStructured(newWordCount);
    m_streamingActiveGroupsBits->ResizeStructured(newWordCount);
    m_streamingGpuStorageGroupCapacity.store(newCapacity, std::memory_order_release);
    uint32_t expected = newCapacity;
    m_pendingStreamingGpuStorageGroupCapacity.compare_exchange_strong(
        expected, 0u, std::memory_order_acq_rel, std::memory_order_acquire);
    m_streamingGpuResizeAckGeneration.fetch_add(1u, std::memory_order_release);
    m_streamingGpuResizeAckGeneration.notify_one();
    RequestStreamingFrameWork();
    return true;
}

void CLodStreamingSystem::InitializeActiveRange(
    MeshManager* meshManager,
    uint32_t begin,
    uint32_t count,
    uint32_t& initializedGroups,
    uint32_t& queuedPinnedGroups) {
    if (meshManager == nullptr) {
        return;
    }

    const uint32_t end = std::min(begin + count, m_streamingStorageGroupCapacity);
    for (uint32_t groupIndex = begin; groupIndex < end; ++groupIndex) {
        const uint32_t word = BitWordAddress(groupIndex);
        if (word >= m_streamingResidencyInitializedBitsCpu.size()) {
            continue;
        }
        const uint32_t mask = BitMask(groupIndex);
        if ((m_streamingActiveGroupsBitsCpu[word] & mask) == 0u ||
            (m_streamingResidencyInitializedBitsCpu[word] & mask) != 0u) {
            continue;
        }
        m_streamingResidencyInitializedBitsCpu[word] |= mask;
        ++initializedGroups;

        const bool pinned = word < m_streamingPinnedGroupsBitsCpu.size()
            && (m_streamingPinnedGroupsBitsCpu[word] & mask) != 0u;
        if (!pinned) {
            SetGroupResidentBit(groupIndex, false);
            continue;
        }

        const MeshManager::CLodGroupStreamingInfo info = meshManager->GetCLodGroupStreamingInfo(groupIndex);
        PreAllocatedPages preAlloc{};
        if (info.valid && info.pageCount > 0u) {
            preAlloc = PreAllocatePagesForGroup(groupIndex, info, meshManager);
            if (preAlloc.segmentCount == 0u) {
                SetGroupResidentBit(groupIndex, false);
                m_streamingResidencyInitializedBitsCpu[word] &= ~mask;
                continue;
            }
            preAlloc.requestGeneration = groupIndex < m_pendingStreamingRequestGenerationByGroup.size()
                ? m_pendingStreamingRequestGenerationByGroup[groupIndex]
                : 0u;
            m_preAllocatedPagesByGroup[groupIndex] = preAlloc;
        }

        const bool queued = meshManager->QueueCLodGroupDiskIO(
            groupIndex,
            preAlloc.segmentNeedsFetch,
            preAlloc.pagesBySegment);
        if (queued) {
            ++queuedPinnedGroups;
            MarkStreamingRequestDiskIo(groupIndex);
            SetGroupResidentBit(groupIndex, false);
        } else {
            if (preAlloc.segmentCount != 0u) {
                ReleasePreAllocatedPages(preAlloc, meshManager);
                m_preAllocatedPagesByGroup.erase(groupIndex);
            }
            if (info.valid && info.pageCount == 0u) {
                SetGroupResidentBit(groupIndex, true);
            } else {
                SetGroupResidentBit(groupIndex, false);
                m_streamingResidencyInitializedBitsCpu[word] &= ~mask;
            }
        }
    }
}

bool CLodStreamingSystem::PublishRetainedUploadBatch() {
    if (!m_retainedUploadBatch) return true;
    if (!m_uploadBatchQueue.TryPush(m_retainedUploadBatch)) {
        TracyPlot("CLodAsyncUpload.BatchQueueFull", static_cast<int64_t>(1));
        static uint64_t lastFullLogMs = 0u;
        const uint64_t nowMs = ClodDiagNowMs();
        if (nowMs >= lastFullLogMs + 1000u) {
            lastFullLogMs = nowMs;
            spdlog::warn(
                "CLod upload batch queue full: depth={} highWater={} fullEvents={} retainedBatch={}",
                m_uploadBatchQueue.Depth(),
                m_uploadBatchQueue.HighWaterMark(),
                m_uploadBatchQueue.FullEvents(),
                m_retainedUploadBatch && m_retainedUploadBatch->ticket
                    ? m_retainedUploadBatch->ticket->batchId : 0u);
        }
        return false;
    }
    TracyPlot("CLodAsyncUpload.BatchQueueFull", static_cast<int64_t>(0));
    m_retainedUploadBatch.reset();
    return true;
}

void CLodStreamingSystem::SealStreamingUploadBatch() {
    if (!m_uploadStream || !m_uploadStream->HasPendingWork() || m_retainedUploadBatch) return;

    auto batch = m_uploadStream->Seal(
        m_uploadBatchGeneration,
        m_nextUploadBatchId.fetch_add(1, std::memory_order_relaxed) + 1u,
        m_residencyGroupsAwaitingUploadFence,
        m_retiringPagesAwaitingUploadFence,
        m_streamingNonResidentBitsQueuedEpoch);
    if (!batch) return;

    m_outstandingUploadBatches.push_back(batch);
    if (!m_uploadBatchQueue.TryPush(batch)) {
        m_retainedUploadBatch = std::move(batch);
    }
    TracyPlot("CLodAsyncUpload.BatchQueueDepth", static_cast<int64_t>(m_uploadBatchQueue.Depth()));
    TracyPlot("CLodAsyncUpload.BatchQueueHighWater", static_cast<int64_t>(m_uploadBatchQueue.HighWaterMark()));
    TracyPlot("CLodAsyncUpload.BatchQueueFullEvents", static_cast<int64_t>(m_uploadBatchQueue.FullEvents()));
}

void CLodStreamingSystem::ObserveUploadBatchTickets() {
    const uint64_t completed = m_streamingUploadCompletionFenceHandle.IsValid()
        ? m_streamingUploadCompletionFenceHandle.GetCompletedValue()
        : 0u;

    for (size_t i = 0; i < m_outstandingUploadBatches.size();) {
        auto& batch = m_outstandingUploadBatches[i];
        if (!batch || !batch->ticket) {
            m_outstandingUploadBatches[i] = std::move(m_outstandingUploadBatches.back());
            m_outstandingUploadBatches.pop_back();
            continue;
        }

        auto state = batch->ticket->state.load(std::memory_order_acquire);
        if (state == CLodUploadTicketState::Cancelled) {
            ++m_cancelledUploadBatchCount;
            batch->submissionObserved = false;
            batch->ticket->completionValue.store(0u, std::memory_order_relaxed);
            batch->ticket->state.store(CLodUploadTicketState::Published, std::memory_order_release);
            if (!m_uploadBatchQueue.TryPush(batch)) {
                m_retainedUploadBatch = batch;
                return;
            }
            ++m_replayedUploadBatchCount;
            state = CLodUploadTicketState::Published;
        }

        if (state == CLodUploadTicketState::Submitted && !batch->submissionObserved) {
            const uint64_t fenceValue = batch->ticket->completionValue.load(std::memory_order_acquire);
            if (fenceValue != 0u) {
                batch->submissionObserved = true;
                if (batch->nonResidentEpoch > m_streamingNonResidentBitsUploadFenceEpoch) {
                    m_streamingNonResidentBitsUploadFenceEpoch = batch->nonResidentEpoch;
                    m_streamingNonResidentBitsUploadFenceValue = fenceValue;
                }
                for (uint32_t groupIndex : batch->affectedGroups) {
                    if (m_pendingResidencyCommitGroups.contains(groupIndex)) {
                        m_pendingResidencyUploadFenceByGroup.insert_or_assign(groupIndex, fenceValue);
                    }
                }
                for (uint32_t page : batch->retiringPages) {
                    if (page >= m_pageState.size() || page >= m_pageReuseUploadFenceValue.size() ||
                        m_pageState[page] != CLodPhysicalPageState::Retiring ||
                        m_pageReuseUploadFenceValue[page] != 0u) {
                        continue;
                    }
                    const uint64_t requiredEpoch = page < m_pageReuseRequiresNonResidentEpoch.size()
                        ? m_pageReuseRequiresNonResidentEpoch[page]
                        : 0u;
                    if (requiredEpoch == 0u || requiredEpoch <= batch->nonResidentEpoch) {
                        m_pageReuseUploadFenceValue[page] = fenceValue;
                    } else {
                        m_retiringPagesAwaitingUploadFence.push_back(page);
                    }
                }
            }
        }

        if (state == CLodUploadTicketState::Submitted && batch->submissionObserved) {
            const uint64_t fenceValue = batch->ticket->completionValue.load(std::memory_order_acquire);
            if (fenceValue != 0u && completed >= fenceValue) {
                batch->ticket->state.store(CLodUploadTicketState::Completed, std::memory_order_release);
                state = CLodUploadTicketState::Completed;
            }
        }
        if (state == CLodUploadTicketState::Completed) {
            if (m_uploadStream) m_uploadStream->Recycle(batch);
            m_outstandingUploadBatches[i] = std::move(m_outstandingUploadBatches.back());
            m_outstandingUploadBatches.pop_back();
            continue;
        }
        ++i;
    }

    TracyPlot("CLodAsyncUpload.OutstandingBatches", static_cast<int64_t>(m_outstandingUploadBatches.size()));
    TracyPlot("CLodAsyncUpload.CancelledBatches", static_cast<int64_t>(m_cancelledUploadBatchCount));
    TracyPlot("CLodAsyncUpload.ReplayedBatches", static_cast<int64_t>(m_replayedUploadBatchCount));
}

void CLodStreamingSystem::RebuildStreamingDomainFromSnapshot(MeshManager* meshManager) {
    ZoneScopedN("CLodStreamingSystem::RebuildStreamingDomainFromSnapshot");
    if (meshManager == nullptr) {
        return;
    }

    MeshManager::CLodStreamingDomainSnapshot snapshot{};
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
    MeshManager* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingDomainEvents::GetMeshManager");
        if (m_getMeshManager) {
            meshManager = m_getMeshManager();
        }
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
        if (event.kind == MeshManager::CLodStreamingDomainEventKind::FullReset) {
            spdlog::warn("CLod streaming: processing full domain reset fallback event");
            RebuildStreamingDomainFromSnapshot(meshManager);
            continue;
        }

        const uint32_t rangeEnd = event.groupsBase + event.groupCount;
        EnsureStreamingStorageCapacity(rangeEnd);
        m_streamingActiveGroupScanCount = std::max(m_streamingActiveGroupScanCount, rangeEnd);

        switch (event.kind) {
        case MeshManager::CLodStreamingDomainEventKind::SharedMeshAdded:
            break;
        case MeshManager::CLodStreamingDomainEventKind::ActiveRangeAdded:
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
        case MeshManager::CLodStreamingDomainEventKind::ActiveRangeRemoved:
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
    if (groupIndex >= m_streamingRequestStateByGroup.size()) {
        return;
    }

    auto& state = m_streamingRequestStateByGroup[groupIndex];
    if (state == StreamingRequestState::None) {
        return;
    }

    if (state == StreamingRequestState::PendingCpu
        && groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()) {
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
    m_readyStreamingCompletionsByGroup.erase(groupIndex);
    if (groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()) {
        m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = UINT32_MAX;
    }
    if (groupIndex < m_pendingStreamingRequestGenerationByGroup.size()) {
        ++m_pendingStreamingRequestGenerationByGroup[groupIndex];
    }
    if (groupIndex < m_waitingForPagesRequestIndexByGroup.size()) {
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

bool CLodStreamingSystem::PopHighestPriorityPendingStreamingRequest(PendingStreamingRequest& outRequest) {
    if (m_pendingStreamingRequests.empty()) {
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

    outRequest = m_pendingStreamingRequests.front();
    const uint32_t groupIndex = outRequest.request.groupGlobalIndex;
    if (groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()
        && m_pendingStreamingRequestHeapIndexByGroup[groupIndex] == 0u) {
        m_pendingStreamingRequestHeapIndexByGroup[groupIndex] = UINT32_MAX;
    }

    if (m_pendingStreamingRequests.size() == 1u) {
        m_pendingStreamingRequests.pop_back();
        return true;
    }

    m_pendingStreamingRequests.front() = m_pendingStreamingRequests.back();
    m_pendingStreamingRequests.pop_back();
    m_pendingStreamingRequestHeapIndexByGroup[m_pendingStreamingRequests.front().request.groupGlobalIndex] = 0u;

    uint32_t index = 0u;
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

void CLodStreamingSystem::SetGroupUsesPinnedStorage(uint32_t groupIndex, bool usesPinnedStorage) {
    if (usesPinnedStorage) {
        m_groupsUsingPinnedStorage.insert(groupIndex);
        return;
    }

    m_groupsUsingPinnedStorage.erase(groupIndex);
}

void CLodStreamingSystem::ApplyDiskStreamingCompletions(MeshManager* meshManager) {
    ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions");

    if (meshManager == nullptr) {
        return;
    }

    std::vector<MeshManager::CLodDiskStreamingCompletion> completions;
    {
        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::DrainCompletions");
        meshManager->DrainCompletedCLodDiskStreamingGroups(completions);
    }
    if (!m_readyStreamingCompletionsByGroup.empty()) {
        completions.reserve(completions.size() + m_readyStreamingCompletionsByGroup.size());
        for (auto& [_, completion] : m_readyStreamingCompletionsByGroup) {
            completions.push_back(std::move(completion));
        }
        m_readyStreamingCompletionsByGroup.clear();
    }

    {
        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::ApplyCompletions");
        for (uint32_t completionIndex = 0; completionIndex < static_cast<uint32_t>(completions.size()); ++completionIndex) {
            auto& completion = completions[completionIndex];
            const uint32_t groupIndex = completion.groupGlobalIndex;
            if (groupIndex >= m_streamingStorageGroupCapacity) {
                continue;
            }
            RecordStreamingCompletion(groupIndex, completion);

            auto clearCompletionRequestState = [this, groupIndex]() {
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
            };

            auto preAllocIt = m_preAllocatedPagesByGroup.find(groupIndex);

            if (completion.success) {
                if (!IsGroupActive(groupIndex)) {
                    if (preAllocIt != m_preAllocatedPagesByGroup.end()) {
                        ReleasePreAllocatedPages(preAllocIt->second, meshManager);
                        m_preAllocatedPagesByGroup.erase(preAllocIt);
                    }
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }
                if (groupIndex >= m_streamingRequestStateByGroup.size() ||
                    m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::DiskIo) {
                    if (preAllocIt != m_preAllocatedPagesByGroup.end()) {
                        ReleasePreAllocatedPages(preAllocIt->second, meshManager);
                        m_preAllocatedPagesByGroup.erase(preAllocIt);
                    }
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

                PreAllocatedPages preAlloc{};
                const bool hadPreAllocation = preAllocIt != m_preAllocatedPagesByGroup.end();
                if (hadPreAllocation) {
                    preAlloc = std::move(preAllocIt->second);
                    m_preAllocatedPagesByGroup.erase(preAllocIt);
                }
                uint32_t expectedPageCount = preAlloc.segmentCount;
                if (!hadPreAllocation) {
                    const auto groupInfo = meshManager->GetCLodGroupStreamingInfo(groupIndex);
                    expectedPageCount = groupInfo.valid ? groupInfo.pageCount : 1u;
                    if (expectedPageCount > 0u) {
                        ZoneScopedN("CLodStreamingSystem::ApplyDiskStreamingCompletions::AllocatePagesAfterReadFallback");
                        preAlloc = PreAllocatePagesForGroup(groupIndex, groupInfo, meshManager);
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
                            m_readyStreamingCompletionsByGroup[groupIndex] = std::move(completion);
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
                        break;
                    }
                }
                if (waitsForPendingSharedPage) {
                    m_preAllocatedPagesByGroup[groupIndex] = std::move(preAlloc);
                    m_readyStreamingCompletionsByGroup[groupIndex] = std::move(completion);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    continue;
                }

                completion.segmentNeedsFetch = preAlloc.segmentNeedsFetch;
                completion.preAllocatedPages = preAlloc.pagesBySegment;
                const bool payloadGpuReady =
                    completion.payloadKind == MeshManager::CLodDiskStreamingPayloadKind::GpuPagesReady;
                const bool payloadUsesExistingPages =
                    completion.payloadKind == MeshManager::CLodDiskStreamingPayloadKind::ReusedExistingPages;
                const bool payloadNeedsCpuUpload =
                    completion.payloadKind == MeshManager::CLodDiskStreamingPayloadKind::CpuPageBlobs;

                // Validate the preallocation immediately before using it. A
                // delayed I/O completion can arrive after one of its physical
                // pages was retired and reused. Previously the CPU upload was
                // queued first and AssignPagesToGroup detected the mismatch
                // afterwards, so stale data could overwrite the page's new
                // mesh owner even though the completion was ultimately rejected.
                bool pageOwnershipValid = true;
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
                if (!pageOwnershipValid) {
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }

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

                PagePool* pool = meshManager->GetCLodPagePool();
                bool payloadValid = true;
                bool queuedPayloadUpload = false;
                for (uint32_t seg = 0; seg < expectedPageCount; ++seg) {
                    const uint32_t page = preAlloc.pagesBySegment[seg];
                    PagePool::PageAllocation allocation{ page, 1u };
                    if (!payloadGpuReady) {
                        completion.pageAllocations[seg] = allocation;
                    }
                    const bool needsFetch = seg < preAlloc.segmentNeedsFetch.size() && preAlloc.segmentNeedsFetch[seg];
                    if (needsFetch && payloadNeedsCpuUpload) {
                        if (pool == nullptr ||
                            seg >= completion.pageBlobs.size() ||
                            completion.pageBlobs[seg].empty() ||
                            completion.pageBlobs[seg].size() > pool->GetPageSize()) {
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
                        pool->UploadToPage(page, 0, completion.pageBlobs[seg].data(), completion.pageBlobs[seg].size());
                        RecordStreamingUploadQueued(groupIndex, static_cast<uint64_t>(completion.pageBlobs[seg].size()));
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
                if (!payloadValid) {
                    ReleasePreAllocatedPages(preAlloc, meshManager);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                    clearCompletionRequestState();
                    continue;
                }
                if ((!queuedPayloadUpload || payloadGpuReady) && completion.fetchedPageCount != 0u) {
                    RecordStreamingUploadQueued(groupIndex, completion.totalStreamedBytes);
                }

                if (!ValidateRenderableCompletion(groupIndex, preAlloc, completion, expectedPageCount)) {
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
                } else {
                    ReleaseGroupResidency(groupIndex, meshManager, true);
                    m_pendingResidencyCommitGroups.erase(groupIndex);
                }
            }
            else {
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

            if (m_pendingResidencyCommitGroups.find(groupIndex) == m_pendingResidencyCommitGroups.end()) {
                clearCompletionRequestState();
            }
        }
    }
}

void CLodStreamingSystem::PollCompletedReadbackSlots() {
    ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots");

    ++m_streamingDiagnosticTick;

    // Drain decoded (groupIndex, priority) pairs produced by the background worker thread.
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
                }
            }
        }
    }

    // Touch the page LRU for all GPU-reported visible groups and their parent chains.
    {
        ZoneScopedN("CLodStreamingSystem::PollCompletedReadbackSlots::TouchVisibleGroupsLru");
        for (const uint32_t word : m_lruTouchedGroupWordsScratch) {
            if (word < m_lruTouchedGroupsBitsScratch.size()) {
                m_lruTouchedGroupsBitsScratch[word] = 0u;
            }
        }
        m_lruTouchedGroupWordsScratch.clear();
        if (m_lruTouchedGroupsBitsScratch.size() < m_streamingActiveGroupsBitsCpu.size()) {
            m_lruTouchedGroupsBitsScratch.resize(m_streamingActiveGroupsBitsCpu.size(), 0u);
        }

        auto touchGroupPagesOnce = [this](uint32_t touchedGroup) {
            const uint32_t wordAddress = BitWordAddress(touchedGroup);
            if (wordAddress >= m_lruTouchedGroupsBitsScratch.size()) {
                return;
            }

            const uint32_t bitMask = BitMask(touchedGroup);
            uint32_t& touchedWord = m_lruTouchedGroupsBitsScratch[wordAddress];
            if ((touchedWord & bitMask) != 0u) {
                return;
            }
            if (touchedWord == 0u) {
                m_lruTouchedGroupWordsScratch.push_back(wordAddress);
            }
            touchedWord |= bitMask;

            auto pagesIt = m_groupOwnedPages.find(touchedGroup);
            if (pagesIt == m_groupOwnedPages.end()) {
                return;
            }

            for (uint32_t page : pagesIt->second) {
                if (page != ~0u) {
                    m_pageLru.Touch(page);
                }
            }
        };

        for (const uint32_t groupIndex : m_usedGroupsBatchScratch) {
            touchGroupPagesOnce(groupIndex);

            uint32_t current = groupIndex;
            for (size_t hop = 0; hop < m_streamingStorageGroupCapacity; ++hop) {
                uint32_t parent = 0;
                if (!TryGetCachedParentGroup(current, parent) || parent == current) {
                    break;
                }

                touchGroupPagesOnce(parent);
                current = parent;
            }
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
            for (const auto& [groupIndex, priority] : m_readbackBatchScratch) {
                CLodStreamingRequest req{};
                req.groupGlobalIndex = groupIndex;
                queuedCount += QueueLoadRequestWithParents(req, priority);
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

void CLodStreamingSystem::StreamingWorkerMain() {
    uint64_t lastProcessed = 0;
    uint64_t observedServiceEpoch = 0;

    while (!m_streamingWorkerQuit.load(std::memory_order_relaxed)) {
        // Epoch pulses may coalesce: a service tick always consumes the latest
        // worker-owned state, so no individual frame notification is lossless data.
        uint64_t requestedEpoch = m_streamingServiceEpoch.load(std::memory_order_acquire);
        if (requestedEpoch == observedServiceEpoch &&
            m_streamingReadbackFenceCounter.load(std::memory_order_acquire) <= lastProcessed &&
            !m_streamingWorkerQuit.load(std::memory_order_acquire)) {
            m_streamingServiceEpoch.wait(requestedEpoch, std::memory_order_acquire);
            requestedEpoch = m_streamingServiceEpoch.load(std::memory_order_acquire);
        }
        if (m_streamingWorkerQuit.load(std::memory_order_relaxed)) break;
        const bool serviceRequested = requestedEpoch != observedServiceEpoch;
        observedServiceEpoch = requestedEpoch;

        const uint64_t discardedThrough =
            m_streamingReadbackDiscardedFenceCounter.load(std::memory_order_acquire);
        if (discardedThrough > lastProcessed) {
            lastProcessed = discardedThrough;
        }

        const uint64_t submittedTarget = m_streamingReadbackFenceCounter.load(std::memory_order_acquire);
        const bool hasFenceWork = submittedTarget > lastProcessed;
        bool readbackReady = false;
        uint64_t decodeTarget = lastProcessed;
        if (hasFenceWork) {
            ZoneScopedN("CLodStreamingWorker::WaitReadbackFence");
            uint64_t completed = m_streamingReadbackFenceHandle.GetCompletedValue();
            if (completed > lastProcessed) {
                decodeTarget = std::min(completed, submittedTarget);
                readbackReady = true;
            }
            // Decode already-completed readbacks instead of waiting for the newest submitted fence.
            // If nothing is complete yet, block on the next unprocessed value. An unsignaled
            // readback fence is a GPU/submission failure and should remain visible as a hard stall.
            if (!readbackReady) {
                const uint64_t waitTarget = lastProcessed + 1u;
                const auto result = m_streamingReadbackFenceHandle.HostWait(waitTarget, UINT32_MAX);
                if (result == rhi::Result::Ok) {
                    completed = m_streamingReadbackFenceHandle.GetCompletedValue();
                    decodeTarget = std::min(completed, submittedTarget);
                    readbackReady = decodeTarget > lastProcessed;
                    if (!readbackReady) {
                        spdlog::error(
                            "CLod StreamingWorker readback fence wait returned before completion: submittedTarget={} waitTarget={} completed={}",
                            submittedTarget,
                            waitTarget,
                            completed);
                    }
                }
                else {
                    spdlog::error(
                        "CLod StreamingWorker readback fence wait failed: submittedTarget={} waitTarget={} result={} completed={}",
                        submittedTarget,
                        waitTarget,
                        rhi::ResultName(result),
                        m_streamingReadbackFenceHandle.GetCompletedValue());
                }
            }
            if (m_streamingWorkerQuit.load(std::memory_order_relaxed)) break;
        }

        if (readbackReady) {
            // Submitted slots are published by the graph thread with release;
            // this worker is their sole decoder and returns them to Free.
            {
                ZoneScopedN("CLodStreamingWorker::DecodeReadbackSlots");

                std::vector<std::pair<uint32_t, uint32_t>> decodedRequests;
                std::vector<uint32_t> sumSeenGroups;
                std::vector<uint32_t> decodedUsedGroups;
                bool decodedAnyCompletedSlot = false;
                const bool sortedFeedback = m_parallelSortAvailable;

                auto beginDecodeGeneration = [](uint32_t& generation, std::vector<uint32_t>& seen) {
                    ++generation;
                    if (generation == 0u) {
                        generation = 1u;
                        std::fill(seen.begin(), seen.end(), 0u);
                    }
                };

                beginDecodeGeneration(m_decodeSeenGeneration, m_decodeSeenGenerationByGroup);
                beginDecodeGeneration(m_decodeUsedSeenGeneration, m_decodeUsedSeenGenerationByGroup);

                for (auto& slot : m_readbackStagingSlots) {
                    if (slot.state.load(std::memory_order_acquire) != ReadbackStagingSlot::State::Submitted ||
                        slot.fenceValue == 0 || slot.fenceValue > decodeTarget) {
                        continue;
                    }
                    auto expected = ReadbackStagingSlot::State::Submitted;
                    if (!slot.state.compare_exchange_strong(
                            expected, ReadbackStagingSlot::State::Decoding,
                            std::memory_order_acq_rel, std::memory_order_acquire)) {
                        continue;
                    }
                    decodedAnyCompletedSlot = true;

                    // Map and read the load counter
                    uint32_t requestCount = 0;
                    {
                        auto apiResource = slot.counterStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            std::memcpy(&requestCount, mapped, sizeof(uint32_t));
                            apiResource.Unmap(0, 0);
                        }
                    }
                    requestCount = std::min<uint32_t>(requestCount, CLodStreamingRequestCapacity);

                    if (requestCount > 0 && slot.requestsStaging) {
                        auto apiResource = slot.requestsStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            const auto* requests = static_cast<const CLodStreamingRequest*>(mapped);
                            for (uint32_t i = 0; i < requestCount; ++i) {
                                const uint32_t groupIndex = requests[i].groupGlobalIndex;
                                const uint32_t priority = UnpackStreamingRequestPriority(requests[i]);
                                if (groupIndex >= m_decodeSeenGenerationByGroup.size()) {
                                    const size_t newSize = static_cast<size_t>(groupIndex) + 1u;
                                    m_decodeSeenGenerationByGroup.resize(newSize, 0u);
                                    m_decodePriorityAccumByGroup.resize(newSize, 0u);
                                }

                                if (m_priorityMode == CLodPriorityMode::Sum || !sortedFeedback) {
                                    if (m_decodeSeenGenerationByGroup[groupIndex] != m_decodeSeenGeneration) {
                                        m_decodeSeenGenerationByGroup[groupIndex] = m_decodeSeenGeneration;
                                        m_decodePriorityAccumByGroup[groupIndex] = priority;
                                        sumSeenGroups.push_back(groupIndex);
                                    }
                                    else if (m_priorityMode == CLodPriorityMode::Sum) {
                                        m_decodePriorityAccumByGroup[groupIndex] += priority;
                                    }
                                    else {
                                        m_decodePriorityAccumByGroup[groupIndex] = std::max(m_decodePriorityAccumByGroup[groupIndex], priority);
                                    }
                                }
                                else if (m_decodeSeenGenerationByGroup[groupIndex] != m_decodeSeenGeneration) {
                                    // Sorted feedback is highest-priority first, so the first occurrence wins.
                                    m_decodeSeenGenerationByGroup[groupIndex] = m_decodeSeenGeneration;
                                    decodedRequests.emplace_back(groupIndex, priority);
                                }
                            }
                            apiResource.Unmap(0, 0);
                        }
                    }

                    // Read the used-groups append buffer (GPU-reported visible groups for LRU touch).
                    uint32_t usedGroupsCount = 0;
                    if (slot.usedGroupsCounterStaging) {
                        auto apiResource = slot.usedGroupsCounterStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            std::memcpy(&usedGroupsCount, mapped, sizeof(uint32_t));
                            apiResource.Unmap(0, 0);
                        }
                    }
                    usedGroupsCount = std::min<uint32_t>(usedGroupsCount, CLodUsedGroupsCapacity);

                    if (usedGroupsCount > 0 && slot.usedGroupsBufferStaging) {
                        auto apiResource = slot.usedGroupsBufferStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            const auto* usedGroups = static_cast<const uint32_t*>(mapped);
                            for (uint32_t i = 0; i < usedGroupsCount; ++i) {
                                const uint32_t groupIndex = usedGroups[i];
                                if (groupIndex >= m_decodeUsedSeenGenerationByGroup.size()) {
                                    m_decodeUsedSeenGenerationByGroup.resize(static_cast<size_t>(groupIndex) + 1u, 0u);
                                }
                                if (m_decodeUsedSeenGenerationByGroup[groupIndex] != m_decodeUsedSeenGeneration) {
                                    m_decodeUsedSeenGenerationByGroup[groupIndex] = m_decodeUsedSeenGeneration;
                                    decodedUsedGroups.push_back(groupIndex);
                                }
                            }
                            apiResource.Unmap(0, 0);
                        }
                    }

                    uint32_t sourceGroupMismatchCount = 0u;
                    if (slot.sourceGroupMismatchCounterStaging) {
                        auto apiResource = slot.sourceGroupMismatchCounterStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            std::memcpy(&sourceGroupMismatchCount, mapped, sizeof(uint32_t));
                            apiResource.Unmap(0, 0);
                        }
                    }

                    if (sourceGroupMismatchCount > 0u && slot.sourceGroupMismatchDetailsStaging) {
                        const uint32_t detailCount =
                            std::min<uint32_t>(sourceGroupMismatchCount, CLodSourceGroupMismatchDetailCapacity);
                        spdlog::error(
                            "CLod source group mismatch telemetry: count={} details_captured={}",
                            sourceGroupMismatchCount,
                            detailCount);

                        auto apiResource = slot.sourceGroupMismatchDetailsStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            const auto* details = static_cast<const CLodSourceGroupMismatchDetail*>(mapped);
                            for (uint32_t i = 0; i < detailCount; ++i) {
                                const CLodSourceGroupMismatchDetail& detail = details[i];
                                spdlog::error(
                                    "CLod source group mismatch detail[{}]: expectedLocal={} foundLocal={} expectedGlobal={} foundGlobal={} metadata={} groupsBase={} expectedSegment={} expectedPage={} expectedMeshlets=[{}, {}) expectedMap={}:{} actualPageLocalMeshlet={} actualMap={}:{} visibleCluster={} unsortedCluster={} instance={} view={} bucketMeshlet={} bucketCount={}",
                                    i,
                                    detail.expectedGroupLocalIndex,
                                    detail.foundGroupLocalIndex,
                                    detail.expectedGroupGlobalIndex,
                                    detail.foundGroupGlobalIndex,
                                    detail.clodMeshMetadataIndex,
                                    detail.groupsBase,
                                    detail.expectedSegmentGlobalIndex,
                                    detail.expectedSegmentPageIndex,
                                    detail.expectedSegmentFirstMeshlet,
                                    detail.expectedSegmentFirstMeshlet + detail.expectedSegmentMeshletCount,
                                    detail.expectedSegmentPageSlabDescriptorIndex,
                                    detail.expectedSegmentPageSlabByteOffset,
                                    detail.pageLocalMeshletIndex,
                                    detail.pageSlabDescriptorIndex,
                                    detail.pageSlabByteOffset,
                                    detail.visibleClusterIndex,
                                    detail.unsortedClusterIndex,
                                    detail.instanceId,
                                    detail.viewId,
                                    detail.bucketMeshletIndex,
                                    detail.bucketCount);
                            }
                            apiResource.Unmap(0, 0);
                        }
                    }

                    slot.state.store(ReadbackStagingSlot::State::Free, std::memory_order_release);
                }

                if (!sumSeenGroups.empty()) {
                    decodedRequests.reserve(decodedRequests.size() + sumSeenGroups.size());
                    for (uint32_t groupIndex : sumSeenGroups) {
                        decodedRequests.emplace_back(groupIndex, m_decodePriorityAccumByGroup[groupIndex]);
                    }
                    std::sort(
                        decodedRequests.begin(),
                        decodedRequests.end(),
                        [](const auto& a, const auto& b) {
                            return a.second > b.second;
                        });
                }

                {
                    m_decodedReadbackBatch.reserve(m_decodedReadbackBatch.size() + decodedRequests.size());
                    if (decodedAnyCompletedSlot) {
                        ++m_decodedUsedGroupsSampleGeneration;
                        m_decodedUsedGroupsBatch.clear();
                    }
                    m_decodedUsedGroupsBatch.reserve(m_decodedUsedGroupsBatch.size() + decodedUsedGroups.size());

                    // Push cross-slot deduplicated results for the streaming service.
                    for (const auto& [groupIndex, priority] : decodedRequests) {
                        m_decodedReadbackBatch.emplace_back(groupIndex, priority);
                    }
                    for (const uint32_t g : decodedUsedGroups) {
                        m_decodedUsedGroupsBatch.push_back(g);
                    }
                }

                if (!decodedRequests.empty() || !decodedUsedGroups.empty()) {
                    RequestStreamingFrameWork();
                }

                TracyPlot("CLodStreaming.Worker.DecodedRequests", static_cast<int64_t>(decodedRequests.size()));
                TracyPlot("CLodStreaming.Worker.DecodedUsedGroups", static_cast<int64_t>(decodedUsedGroups.size()));
            }

            lastProcessed = decodeTarget;
        }

        const uint64_t latestEpoch = m_streamingServiceEpoch.load(std::memory_order_acquire);
        if (serviceRequested || latestEpoch != observedServiceEpoch) {
            observedServiceEpoch = latestEpoch;
            ZoneScopedN("CLodStreamingWorker::RunStreamingServiceWork");
            m_streamingServiceRunning.store(true, std::memory_order_release);
            RunStreamingServiceWork();
            m_streamingServiceRunning.store(false, std::memory_order_release);
        }
    }
}

void CLodStreamingSystem::ProcessStreamingRequestsBudgeted() {
    ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted");

    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::RefreshBudget");
        if (m_getStreamingCpuUploadBudgetRequests) {
            m_streamingCpuUploadBudgetRequests = std::max(m_getStreamingCpuUploadBudgetRequests(), 1u);
        }
        m_streamingCpuUploadBudgetRequests = std::max(m_streamingCpuUploadBudgetRequests, 1u);
    }
    const uint32_t budget = m_streamingCpuUploadBudgetRequests;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ConfigureEvictionBudget");
        m_pagePopEvictionsThisUpdate = 0u;
        const uint32_t requestScaledEvictionBudget = std::clamp<uint32_t>(budget / 8u, 16u, 512u);
        const uint32_t backlogScaledEvictionBudget =
            std::clamp<uint32_t>(m_streamingRequestsInProgressCount / 16u, 16u, 512u);
        m_pagePopEvictionBudgetThisUpdate = std::max<uint32_t>(
            32u,
            std::max(requestScaledEvictionBudget, backlogScaledEvictionBudget));
    }
    CLodStreamingOperationStats frameStats{};

    MeshManager* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::GetMeshManager");
        if (m_getMeshManager) {
            meshManager = m_getMeshManager();
        }
    }

    const uint64_t armedDirectStorageFence =
        m_directStorageArmedLaunchFenceValue.load(std::memory_order_acquire);
    if (meshManager != nullptr && armedDirectStorageFence != 0) {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::PollDirectStorageLaunchFence");
        const uint64_t armedFenceValue = armedDirectStorageFence;
        const uint64_t completedFenceValue = m_directStorageLaunchFenceHandle.GetCompletedValue();
        TracyPlot(
            "CLodStreaming.DirectStorageLaunchFencePending",
            static_cast<int64_t>(completedFenceValue < armedFenceValue ? 1 : 0));
        if (completedFenceValue < armedFenceValue) {
            // Do not block this worker for the render queue. The per-frame
            // publisher will request another service tick after the signal,
            // while returning here keeps the armed launch set frozen.
            return;
        }

        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::LaunchArmedDirectStorageBatch");
            meshManager->LaunchPendingCLodDirectStorageUploads(
                m_directStorageLaunchFenceHandle,
                armedFenceValue);
            m_directStorageArmedLaunchFenceValue.store(0u, std::memory_order_release);
        }
    }

    if (meshManager != nullptr) {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance");
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::InitializePageLru");
            InitializePageLru(meshManager);
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::DrainRetiredPhysicalPages");
            DrainRetiredPhysicalPages(meshManager);
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::RequeueWaitingForPagesRequests");
            RequeueWaitingForPagesRequests(std::max<uint32_t>(budget, 64u));
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::CommitPendingResidencyPromotions");
            CommitPendingResidencyPromotions();
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::ProcessDiskStreamingIO");
            {
                ZoneScopedN("CLodStreamingWorker::ProcessDiskStreamingIO");
                meshManager->ProcessCLodDiskStreamingIO();
            }
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::ApplyDiskStreamingCompletions");
            ApplyDiskStreamingCompletions(meshManager);
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::ReconcileStaleDiskIoRequests");
            ReconcileStaleDiskIoRequests(meshManager);
        }
    }

    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages");
        TracyPlot("CLodStreaming.Protection.UsedGroupWords", static_cast<int64_t>(m_usedGroupsWordsCpu.size()));
        TracyPlot("CLodStreaming.Protection.OwnedGroups", static_cast<int64_t>(m_groupOwnedPages.size()));
        TracyPlot("CLodStreaming.Protection.PreallocatedGroups", static_cast<int64_t>(m_preAllocatedPagesByGroup.size()));
        TracyPlot("CLodStreaming.Protection.PendingCommitGroups", static_cast<int64_t>(m_pendingResidencyCommitGroups.size()));
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::Reset");
            BeginPageProtectionUpdate();
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::UsedGroups");
            for (uint32_t wordIndex : m_usedGroupsWordsCpu) {
                if (wordIndex >= m_usedGroupsBitsCpu.size()) {
                    continue;
                }
                uint32_t bits = m_usedGroupsBitsCpu[wordIndex];
                while (bits != 0u) {
                    const uint32_t bit = static_cast<uint32_t>(std::countr_zero(bits));
                    bits &= bits - 1u;
                    ProtectGroupAndAncestors((wordIndex << 5u) | bit);
                }
            }
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::RecentlyUsedOwnedGroups");
            const uint64_t protectedUsedWindow = static_cast<uint64_t>(std::max<uint32_t>(m_streamingReadbackRingSize, 1u) + 1u);
            for (const auto& [groupIndex, _] : m_groupOwnedPages) {
                if (groupIndex < m_groupLastUsedTick.size() &&
                    m_groupLastUsedTick[groupIndex] != 0u &&
                    m_streamingDiagnosticTick <= m_groupLastUsedTick[groupIndex] + protectedUsedWindow) {
                    ProtectGroupAndAncestors(groupIndex);
                }
            }
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::PreallocatedGroups");
            for (const auto& [groupIndex, _] : m_preAllocatedPagesByGroup) {
                ProtectGroupAndAncestors(groupIndex);
            }
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::PendingCommitGroups");
            for (uint32_t groupIndex : m_pendingResidencyCommitGroups) {
                ProtectGroupAndAncestors(groupIndex);
            }
        }
    }

    auto* pool = meshManager ? meshManager->GetCLodPagePool() : nullptr;
    const uint64_t pageSize = pool ? pool->GetPageSize() : 0u;

    struct QueuedStreamingCandidate {
        uint32_t groupIndex = 0u;
        MeshManager::CLodGroupDiskIOBatchRequest request;
    };
    std::vector<QueuedStreamingCandidate> diskIoBatch;
    diskIoBatch.reserve(budget);

    uint32_t processed = 0;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::SelectAndPrepareRequests");
        {
            ZoneScopedN("CLodStreamingWorker::SelectAndPrepareRequests");
            while (processed < budget && !m_pendingStreamingRequests.empty()) {
            PendingStreamingRequest pending{};
            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::PopPendingRequest");
                if (!PopHighestPriorityPendingStreamingRequest(pending)) {
                    break;
                }
            }

            const uint32_t groupIndex = pending.request.groupGlobalIndex;
            const uint32_t priority = pending.priority;
            ProtectGroupAndAncestors(groupIndex);
            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ValidatePendingRequest");
                if (groupIndex >= m_streamingStorageGroupCapacity) {
                    EnsureStreamingStorageCapacity(groupIndex + 1u);
                }

                if (groupIndex >= m_streamingRequestStateByGroup.size()
                    || m_streamingRequestStateByGroup[groupIndex] != StreamingRequestState::PendingCpu
                    || priority != GetPendingLoadPriority(groupIndex)
                    || groupIndex >= m_pendingStreamingRequestGenerationByGroup.size()
                    || pending.generation != m_pendingStreamingRequestGenerationByGroup[groupIndex]) {
                    if (groupIndex < m_streamingRequestStateByGroup.size()
                        && m_streamingRequestStateByGroup[groupIndex] == StreamingRequestState::PendingCpu
                        && groupIndex < m_pendingStreamingRequestHeapIndexByGroup.size()
                        && m_pendingStreamingRequestHeapIndexByGroup[groupIndex] == UINT32_MAX) {
                        ClearStreamingRequestInProgress(groupIndex);
                        ClearPendingLoadPriority(groupIndex);
                    }
                    continue;
                }
            }

            // Load path
            frameStats.loadRequested++;
            frameStats.loadUnique++;

            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ActiveResidentChecks");
                // Skip groups that are no longer in the active domain.
                if (!IsGroupActive(groupIndex)) {
                    ClearStreamingRequestInProgress(groupIndex);
                    ClearPendingLoadPriority(groupIndex);
                    processed++;
                    continue;
                }

                if (IsGroupResident(groupIndex)) {
                    TouchGroupPages(groupIndex);
                    ClearStreamingRequestInProgress(groupIndex);
                    ClearPendingLoadPriority(groupIndex);
                    processed++;
                    continue;
                }
            }

            if (meshManager == nullptr) {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::CpuFallbackCommit");
                if (SetGroupResidentBit(groupIndex, true)) {
                    frameStats.loadApplied++;
                }
                ClearStreamingRequestInProgress(groupIndex);
                ClearPendingLoadPriority(groupIndex);
                processed++;
                continue;
            }

            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::BuildDiskIoCandidate");
                const CLodCache::GroupPayloadLayoutMetadata* prefetchedLayout = nullptr;
                auto paIt = m_preAllocatedPagesByGroup.find(groupIndex);
                if (paIt == m_preAllocatedPagesByGroup.end()) {
                    const auto info = meshManager->GetCLodGroupStreamingInfo(groupIndex);
                    const uint32_t expectedPageCount = info.valid ? info.pageCount : 1u;
                    PreAllocatedPages preAlloc{};
                    if (expectedPageCount > 0u) {
                        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::BuildDiskIoCandidate::PreAllocatePages");
                        preAlloc = PreAllocatePagesForGroup(groupIndex, info, meshManager);
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
                            ParkStreamingRequestWaitingForPages(pending);
                            processed++;
                            continue;
                        }
                    }
                    paIt = m_preAllocatedPagesByGroup.emplace(groupIndex, std::move(preAlloc)).first;
                }

                auto prefetchedIt = m_prefetchedChildLayoutsByGroup.find(groupIndex);
                if (prefetchedIt != m_prefetchedChildLayoutsByGroup.end() && prefetchedIt->second.layout.IsValid()) {
                    prefetchedLayout = &prefetchedIt->second.layout;
                    spdlog::debug(
                        "CLod streaming: queueing group {} with prefetched child header metadata from owner {}",
                        groupIndex,
                        prefetchedIt->second.ownerGroupIndex);
                }

                QueuedStreamingCandidate candidate{};
                candidate.groupIndex = groupIndex;
                candidate.request.groupGlobalIndex = groupIndex;
                candidate.request.segmentNeedsFetch = paIt->second.segmentNeedsFetch;
                candidate.request.preAllocatedPages = paIt->second.pagesBySegment;
                candidate.request.priority = priority;
                if (prefetchedLayout != nullptr && prefetchedLayout->IsValid()) {
                    candidate.request.prefetchedLayout = *prefetchedLayout;
                }
                if (meshManager->IsCLodStreamingDirectStorageEnabled()) {
                    meshManager->GetCLodChildGroups(groupIndex, candidate.request.childLayoutPrefetchGroups);
                }
                diskIoBatch.push_back(std::move(candidate));
            }

            processed++;
            }
        }
    }

    if (meshManager != nullptr && !diskIoBatch.empty()) {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::QueueDiskIoBatch");
        {
            ZoneScopedN("CLodStreamingWorker::QueueDiskIoBatch");
            TracyPlot("CLodStreaming.Service.DiskIoBatchSize", static_cast<int64_t>(diskIoBatch.size()));

            std::vector<MeshManager::CLodGroupDiskIOBatchRequest> batchRequests;
            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::QueueDiskIoBatch::BuildRequests");
                batchRequests.reserve(diskIoBatch.size());
                for (const auto& candidate : diskIoBatch) {
                    batchRequests.push_back(candidate.request);
                }
            }

            std::vector<bool> queuedByRequest;
            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::QueueDiskIoBatch::Submit");
                meshManager->QueueCLodGroupDiskIOBatch(batchRequests, &queuedByRequest);
            }

            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::QueueDiskIoBatch::ApplyResults");
                for (uint32_t i = 0; i < static_cast<uint32_t>(diskIoBatch.size()); ++i) {
                    const uint32_t groupIndex = diskIoBatch[i].groupIndex;
                    const bool queued = i < queuedByRequest.size() && queuedByRequest[i];
                    if (queued) {
                        MarkStreamingRequestDiskIo(groupIndex);
                        continue;
                    }

                    frameStats.loadFailed++;
                    auto paIt = m_preAllocatedPagesByGroup.find(groupIndex);
                    if (paIt != m_preAllocatedPagesByGroup.end()) {
                        ReleasePreAllocatedPages(paIt->second, meshManager);
                        m_preAllocatedPagesByGroup.erase(paIt);
                    }
                    ClearStreamingRequestInProgress(groupIndex);
                    ClearPendingLoadPriority(groupIndex);
                }

            }
            {
                ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::QueueDiskIoBatch::DispatchQueuedIo");
                meshManager->ProcessCLodDiskStreamingIO();
            }
        }
    }

    if (meshManager != nullptr) {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::CollectDebugStats");
        const auto debugStats = meshManager->GetCLodStreamingDebugStats();
        frameStats.decodedRequests = m_streamingDiagnosticsDecodedRequestsThisFrame;
        frameStats.queuedLoadRequests = m_streamingDiagnosticsQueuedLoadRequestsThisFrame;
        frameStats.residentGroups = debugStats.residentGroups;
        frameStats.residentAllocations = debugStats.residentAllocations;
        frameStats.queuedRequests = debugStats.queuedRequests;
        frameStats.queuedOrInFlightGroups = debugStats.queuedOrInFlightGroups;
        frameStats.dispatchedOrInFlightGroups = debugStats.dispatchedOrInFlightGroups;
        frameStats.completedResults = debugStats.completedResults;
        frameStats.pendingDirectStorageLaunches = debugStats.pendingDirectStorageLaunches;
        frameStats.pendingDirectStorageUploads = debugStats.pendingDirectStorageUploads;
        frameStats.residentAllocationBytes = debugStats.residentAllocationBytes;
        frameStats.completedResultBytes = debugStats.completedResultBytes;
        if (debugStats.pendingDirectStorageLaunches != 0u) {
            m_directStorageLaunchRequested.store(true, std::memory_order_release);
        }
        frameStats.streamedBytesThisFrame = debugStats.totalStreamedBytes - m_prevTotalStreamedBytes;
        m_prevTotalStreamedBytes = debugStats.totalStreamedBytes;
    }

    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::PublishStats");
        AccumulateStreamingDiagnostics(frameStats);
        PublishCLodStreamingOperationStats(frameStats);
        m_streamingDiagnosticsDecodedRequestsThisFrame = 0u;
        m_streamingDiagnosticsQueuedLoadRequestsThisFrame = 0u;
        m_streamingDiagnosticsDuplicateRequestsThisFrame = 0u;
        m_streamingDiagnosticsPreallocationDeferralsThisFrame = 0u;
        m_streamingDiagnosticsPromotionDeferralsThisFrame = 0u;
        m_streamingDiagnosticsCompletionSuccessThisFrame = 0u;
        m_streamingDiagnosticsCompletionFailedThisFrame = 0u;
        m_streamingDiagnosticsUploadQueuedGroupsThisFrame = 0u;
        m_streamingDiagnosticsUploadQueuedBytesThisFrame = 0u;
        m_streamingDiagnosticsRequestToUploadSamplesThisFrame = 0u;
        m_streamingDiagnosticsRequestToUploadSumThisFrame = 0u;
        m_streamingDiagnosticsRequestToUploadWorstThisFrame = 0u;
        m_streamingDiagnosticsRequestToUploadWorstGroupThisFrame = 0u;
        m_streamingDiagnosticsRequestToResidentSamplesThisFrame = 0u;
        m_streamingDiagnosticsRequestToResidentSumThisFrame = 0u;
        m_streamingDiagnosticsRequestToResidentWorstThisFrame = 0u;
        m_streamingDiagnosticsRequestToResidentWorstGroupThisFrame = 0u;
        m_streamingDiagnosticsDiskQueueToCompleteSamplesThisFrame = 0u;
        m_streamingDiagnosticsDiskQueueToCompleteSumThisFrame = 0u;
        m_streamingDiagnosticsDiskQueueToCompleteWorstThisFrame = 0u;
        m_streamingDiagnosticsUploadToResidentSamplesThisFrame = 0u;
        m_streamingDiagnosticsUploadToResidentSumThisFrame = 0u;
        m_streamingDiagnosticsUploadToResidentWorstThisFrame = 0u;
        m_streamingDiagnosticsCommitToResidentSamplesThisFrame = 0u;
        m_streamingDiagnosticsCommitToResidentSumThisFrame = 0u;
        m_streamingDiagnosticsCommitToResidentWorstThisFrame = 0u;
    }
}
