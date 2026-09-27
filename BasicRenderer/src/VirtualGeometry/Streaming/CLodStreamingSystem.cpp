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
    std::shared_ptr<org::Buffer> keyScratch;
    std::shared_ptr<org::Buffer> payloadScratch;
    std::shared_ptr<org::Buffer> sumTable;
    std::shared_ptr<org::Buffer> reduceTable;
    std::shared_ptr<org::Buffer> constants;
    std::shared_ptr<org::Buffer> countScatterArgs;
    std::shared_ptr<org::Buffer> reduceScanArgs;
};

namespace {
    struct CLodStreamingUploadSnapshotKey {
        uint64_t dstResourceId = 0;
        uint64_t srcResourceId = 0;
        size_t dstOffset = 0;
        size_t srcOffset = 0;
        size_t size = 0;

        bool operator==(const CLodStreamingUploadSnapshotKey&) const = default;
    };

    std::vector<CLodStreamingUploadSnapshotKey> MakeStreamingUploadSnapshotKey(
        const std::vector<org::StreamingUploadDescriptor>& uploads) {
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

    struct CLodStructuralStreamingUploadFrameData {
        struct Copy {
            org::PreparedResourceReference destination{};
            org::PreparedResourceReference source{};
            uint64_t destinationOffset = 0;
            uint64_t sourceOffset = 0;
            uint64_t size = 0;
            std::shared_ptr<org::TrackedUploadTicket> ticket;
        };
        std::vector<Copy> copies;
    };

    class CLodStructuralStreamingUploadPass final
        : public org::TypedRenderGraphPass<CLodStructuralStreamingUploadPass,
              CLodStructuralStreamingUploadFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        using ConsumeUploadsFn = std::function<std::vector<org::StreamingUploadDescriptor>()>;

        explicit CLodStructuralStreamingUploadPass(ConsumeUploadsFn consumeUploads)
            : m_consumeUploads(std::move(consumeUploads)) {}

        bool DeclaredResourcesChanged() const override {
            ZoneScopedN("CLodStructuralStreamingUploadPass::DeclaredResourcesChanged::SnapshotOnly");

            if (!m_uploadSnapshotValid) {
                m_uploadSnapshot = m_consumeUploads ? m_consumeUploads() : std::vector<org::StreamingUploadDescriptor>{};
                m_uploadSnapshotValid = true;
            }

            org::StreamingUploadInputs nextInputs{};
            nextInputs.uploads = m_uploadSnapshot;

            auto nextKey = MakeStreamingUploadSnapshotKey(nextInputs.uploads);
            const bool changed = !m_initialized || nextKey != m_snapshotKey;
            m_initialized = true;
            m_snapshotKey = std::move(nextKey);
            m_inputs = std::move(nextInputs);
            return changed;
        }

        bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
        bool DeclarationsProvidedByImmediateCommands() const noexcept override { return true; }

        void Declare(org::PassBuilder& builder) {
            for (const auto& upload : m_inputs.uploads) {
                if (!upload.dstResource || !upload.srcUploadBuffer || upload.size == 0) continue;
                builder.WithCopySource(upload.srcUploadBuffer);
                builder.WithCopyDest(upload.dstResource);
            }
            builder.PreferQueue(org::QueueKind::Copy);
        }

        CLodStructuralStreamingUploadFrameData Prepare(const org::PassPrepareContext& preparation) {
            if (!m_uploadSnapshotValid) {
                m_uploadSnapshot = m_consumeUploads
                    ? m_consumeUploads() : std::vector<org::StreamingUploadDescriptor>{};
                m_uploadSnapshotValid = true;
                m_inputs.uploads = m_uploadSnapshot;
            }
            CLodStructuralStreamingUploadFrameData frame;
            frame.copies.reserve(m_inputs.uploads.size());
            for (const auto& upload : m_inputs.uploads) {
                if (upload.ticket && upload.ticket->state.load(std::memory_order_acquire) ==
                    org::TrackedUploadTicketState::Cancelled) {
                    continue;
                }
                if (!upload.dstResource || !upload.srcUploadBuffer || upload.size == 0) {
                    continue;
                }
                frame.copies.push_back({
                    preparation.CaptureResource(upload.dstResource->GetGlobalResourceID()),
                    preparation.CaptureResource(upload.srcUploadBuffer->GetGlobalResourceID()),
                    upload.dstOffset, upload.srcOffset, upload.size, upload.ticket});
            }
            m_uploadSnapshot.clear();
            m_uploadSnapshotValid = false;
            return frame;
        }

        static void Record(const CLodStructuralStreamingUploadFrameData& frame,
            org::PassRecordContext& recording) {
            for (const auto& copy : frame.copies) {
                if (copy.ticket && copy.ticket->state.load(std::memory_order_acquire) ==
                    org::TrackedUploadTicketState::Cancelled) continue;
                recording.Commands().CopyBufferRegion(
                    recording.Resolve(copy.destination).GetHandle(), copy.destinationOffset,
                    recording.Resolve(copy.source).GetHandle(), copy.sourceOffset, copy.size);
            }
        }

    private:
        ConsumeUploadsFn m_consumeUploads;
        mutable std::vector<org::StreamingUploadDescriptor> m_uploadSnapshot;
        mutable org::StreamingUploadInputs m_inputs;
        mutable std::vector<CLodStreamingUploadSnapshotKey> m_snapshotKey;
        mutable bool m_uploadSnapshotValid = false;
        mutable bool m_initialized = false;
    };

    struct CLodAsyncUploadSnapshot {
        std::vector<std::shared_ptr<CLodUploadBatch>> batches;
        std::vector<std::shared_ptr<org::Resource>> destinations;
        rhi::Timeline completionTimeline;
        uint64_t completionValue = 0;
    };

    struct CLodStructuralAsyncUploadFrameData {
        struct Copy {
            org::PreparedResourceReference destination{}, source{};
            uint64_t destinationOffset = 0, sourceOffset = 0, size = 0;
        };
        std::vector<Copy> copies;
    };

    class CLodUploadSignalReservation final : public org::PreparedLifecycleEffect {
    public:
        CLodUploadSignalReservation(rhi::Timeline timeline, uint64_t value,
            std::vector<std::shared_ptr<CLodUploadBatch>> batches)
            : m_signal{timeline, value}, m_batches(std::move(batches)) {}
        std::span<const org::ExternalTimelinePoint> SignalsAfterCompletion() const override {
            return {&m_signal, 1u};
        }
        void Submitted(org::SubmissionContext) const override {
            (void)m_resolved.exchange(true, std::memory_order_acq_rel);
        }
        void Abandoned(org::AbandonReason) const override {
            if (m_resolved.exchange(true)) return;
            for (const auto& batch : m_batches) {
                if (!batch || !batch->ticket) continue;
                auto expected = CLodUploadTicketState::Submitted;
                batch->ticket->state.compare_exchange_strong(expected,
                    CLodUploadTicketState::Cancelled, std::memory_order_acq_rel);
            }
        }
    private:
        org::ExternalTimelinePoint m_signal{};
        std::vector<std::shared_ptr<CLodUploadBatch>> m_batches;
        mutable std::atomic<bool> m_resolved{false};
    };

    class CLodStructuralAsyncUploadPass final
        : public org::TypedRenderGraphPass<CLodStructuralAsyncUploadPass,
              CLodStructuralAsyncUploadFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        using TryAcquireSnapshotFn = std::function<bool(CLodAsyncUploadSnapshot&)>;
        using SubmitSnapshotFn = std::function<org::PassReturn(CLodAsyncUploadSnapshot&)>;

        CLodStructuralAsyncUploadPass(
            TryAcquireSnapshotFn tryAcquireSnapshot,
            SubmitSnapshotFn submitSnapshot)
            : m_tryAcquireSnapshot(std::move(tryAcquireSnapshot))
            , m_submitSnapshot(std::move(submitSnapshot)) {}

        bool DeclaredResourcesChanged() const override {
            ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::SnapshotOnly");

            CancelClaimedSnapshot();
            CLodAsyncUploadSnapshot nextSnapshot{};
            bool armed = false;
            {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::AcquireSnapshot");
                armed = m_tryAcquireSnapshot && m_tryAcquireSnapshot(nextSnapshot);
            }
            std::vector<uint64_t> nextResourceIds;
            if (armed) {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::BuildResourceIds");
                for (const auto& batch : nextSnapshot.batches) {
                    if (!batch) continue;
                    nextResourceIds.reserve(nextResourceIds.size() + batch->copies.size() * 2u);
                    for (const auto& copy : batch->copies) {
                        if (!copy.destination || !copy.staging || copy.size == 0u) continue;
                        nextResourceIds.push_back(copy.destination->GetGlobalResourceID());
                        nextResourceIds.push_back(copy.staging->GetGlobalResourceID());
                    }
                }
            }

            bool changed = false;
            {
                ZoneScopedN("CLodStructuralAsyncUploadPass::DeclaredResourcesChanged::InstallSnapshot");
                changed = !m_initialized || nextResourceIds != m_declaredResourceIds;
                m_initialized = true;
                m_declaredResourceIds = std::move(nextResourceIds);
                m_armed = armed;
                m_snapshot = std::move(nextSnapshot);
            }
            return changed;
        }

        bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
        void Declare(org::PassBuilder& builder) {
            ZoneScopedN("CLodStructuralAsyncUploadPass::DeclareResourceUsages");
            for (const auto& batch : m_snapshot.batches) {
                if (!batch) continue;
                for (const auto& copy : batch->copies) {
                    if (!copy.destination || !copy.staging || copy.size == 0u) continue;
                    builder.WithCopySource(copy.staging);
                    builder.WithCopyDest(copy.destination);
                }
            }
            builder.PreferQueue(org::QueueKind::Graphics);
        }

        CLodStructuralAsyncUploadFrameData Prepare(const org::PassPrepareContext& preparation) {
            CLodStructuralAsyncUploadFrameData data;
            if (!m_armed) return data;
            for (const auto& batch : m_snapshot.batches) {
                if (!batch) continue;
                for (const auto& copy : batch->copies) {
                    if (!copy.destination || !copy.staging || !copy.size) continue;
                    data.copies.push_back({
                        preparation.CaptureResource(copy.destination->GetGlobalResourceID()),
                        preparation.CaptureResource(copy.staging->GetGlobalResourceID()),
                        copy.destinationOffset, copy.stagingOffset, copy.size});
                }
            }
            if (data.copies.empty() || !m_submitSnapshot) return data;
            auto submission = m_submitSnapshot(m_snapshot);
            if (submission.fence || submission.fenceValue ||
                submission.externalSignalsAfterCompletion.size() != 1u) return {};
            const auto completionValue = m_snapshot.completionValue;
            preparation.Reserve(std::make_shared<CLodUploadSignalReservation>(
                m_snapshot.completionTimeline, completionValue, m_snapshot.batches));
            m_armed = false;
            return data;
        }

        static void Record(const CLodStructuralAsyncUploadFrameData& data,
            org::PassRecordContext& recording) {
            for (const auto& copy : data.copies) recording.Commands().CopyBufferRegion(
                recording.Resolve(copy.destination).GetHandle(), copy.destinationOffset,
                recording.Resolve(copy.source).GetHandle(), copy.sourceOffset, copy.size);
        }

        void ShutdownPass() { CancelClaimedSnapshot(); }

    private:
        TryAcquireSnapshotFn m_tryAcquireSnapshot;
        SubmitSnapshotFn m_submitSnapshot;
        mutable CLodAsyncUploadSnapshot m_snapshot;
        mutable std::vector<uint64_t> m_declaredResourceIds;
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
        CLodStreamingReadbackSources inputs;
        std::shared_ptr<org::Buffer> counterStaging;
        std::shared_ptr<org::Buffer> requestsStaging;
        std::shared_ptr<org::Buffer> usedGroupsCounterStaging;
        std::shared_ptr<org::Buffer> usedGroupsBufferStaging;
        std::shared_ptr<org::Buffer> sourceGroupMismatchCounterStaging;
        std::shared_ptr<org::Buffer> sourceGroupMismatchDetailsStaging;
        std::shared_ptr<org::Buffer> virtualShadowDependencyCountStaging;
        std::shared_ptr<org::Buffer> virtualShadowDependenciesStaging;
        uint32_t selectedSlot = UINT32_MAX;
    };

    struct CLodStructuralReadbackFrameData {
        struct Copy {
            org::PreparedResourceReference destination{}, source{};
            uint64_t bytes = 0;
        };
        std::vector<Copy> copies;
    };

    class CLodReadbackSignalReservation final : public org::PreparedLifecycleEffect {
    public:
        CLodReadbackSignalReservation(org::ExternalTimelinePoint signal,
            std::function<void()> cancel)
            : m_signal(signal), m_cancel(std::move(cancel)) {}
        std::span<const org::ExternalTimelinePoint> SignalsAfterCompletion() const override {
            return {&m_signal, 1u};
        }
        void Submitted(org::SubmissionContext) const override {
            (void)m_resolved.exchange(true, std::memory_order_acq_rel);
        }
        void Abandoned(org::AbandonReason) const override {
            if (!m_resolved.exchange(true) && m_cancel) m_cancel();
        }
    private:
        org::ExternalTimelinePoint m_signal{};
        std::function<void()> m_cancel;
        mutable std::atomic<bool> m_resolved{false};
    };

    class CLodStructuralStreamingReadbackCopyPass final
        : public org::TypedRenderGraphPass<CLodStructuralStreamingReadbackCopyPass,
              CLodStructuralReadbackFrameData>,
          public org::IDynamicDeclaredResources {
    public:
        using TryAcquireSnapshotFn = std::function<bool(CLodStreamingReadbackSnapshot&)>;
        using CompleteSnapshotFn = std::function<org::PassReturn(uint32_t)>;
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
                    nextSnapshot.inputs.virtualShadowDependencyCountSource ? nextSnapshot.inputs.virtualShadowDependencyCountSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.inputs.virtualShadowDependenciesSource ? nextSnapshot.inputs.virtualShadowDependenciesSource->GetGlobalResourceID() : 0ull,
                    nextSnapshot.virtualShadowDependencyCountStaging ? nextSnapshot.virtualShadowDependencyCountStaging->GetGlobalResourceID() : 0ull,
                    nextSnapshot.virtualShadowDependenciesStaging ? nextSnapshot.virtualShadowDependenciesStaging->GetGlobalResourceID() : 0ull,
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

        bool RequiresPassRebindAfterDeclarationRefresh() const noexcept override { return false; }
        void Declare(org::PassBuilder& builder) {
            DeclareCopy(builder, m_snapshot.counterStaging, m_snapshot.inputs.counterSource);
            DeclareCopy(builder, m_snapshot.requestsStaging, m_snapshot.inputs.requestsSource);
            DeclareCopy(builder, m_snapshot.usedGroupsCounterStaging, m_snapshot.inputs.usedGroupsCounterSource);
            DeclareCopy(builder, m_snapshot.usedGroupsBufferStaging, m_snapshot.inputs.usedGroupsBufferSource);
            DeclareCopy(builder, m_snapshot.sourceGroupMismatchCounterStaging, m_snapshot.inputs.sourceGroupMismatchCounterSource);
            DeclareCopy(builder, m_snapshot.sourceGroupMismatchDetailsStaging, m_snapshot.inputs.sourceGroupMismatchDetailsSource);
            DeclareCopy(builder, m_snapshot.virtualShadowDependencyCountStaging, m_snapshot.inputs.virtualShadowDependencyCountSource);
            DeclareCopy(builder, m_snapshot.virtualShadowDependenciesStaging, m_snapshot.inputs.virtualShadowDependenciesSource);
            builder.PreferQueue(org::QueueKind::Graphics);
        }

        CLodStructuralReadbackFrameData Prepare(const org::PassPrepareContext& preparation) {
            CLodStructuralReadbackFrameData frame;
            if (!m_armed || !m_completeSnapshot) return frame;
            CaptureCopy(frame, preparation, m_snapshot.counterStaging, m_snapshot.inputs.counterSource);
            CaptureCopy(frame, preparation, m_snapshot.requestsStaging, m_snapshot.inputs.requestsSource);
            CaptureCopy(frame, preparation, m_snapshot.usedGroupsCounterStaging, m_snapshot.inputs.usedGroupsCounterSource);
            CaptureCopy(frame, preparation, m_snapshot.usedGroupsBufferStaging, m_snapshot.inputs.usedGroupsBufferSource);
            CaptureCopy(frame, preparation, m_snapshot.sourceGroupMismatchCounterStaging, m_snapshot.inputs.sourceGroupMismatchCounterSource);
            CaptureCopy(frame, preparation, m_snapshot.sourceGroupMismatchDetailsStaging, m_snapshot.inputs.sourceGroupMismatchDetailsSource);
            CaptureCopy(frame, preparation, m_snapshot.virtualShadowDependencyCountStaging, m_snapshot.inputs.virtualShadowDependencyCountSource);
            CaptureCopy(frame, preparation, m_snapshot.virtualShadowDependenciesStaging, m_snapshot.inputs.virtualShadowDependenciesSource);
            const uint32_t selectedSlot = m_snapshot.selectedSlot;
            org::PassReturn ret = m_completeSnapshot(selectedSlot);
            if (ret.fence && ret.fenceValue) {
                preparation.Reserve(std::make_shared<CLodReadbackSignalReservation>(
                    org::ExternalTimelinePoint{*ret.fence, ret.fenceValue},
                    [cancel = m_cancelSnapshot, selectedSlot] {
                        if (cancel) cancel(selectedSlot);
                    }));
            }
            m_armed = false;
            return frame;
        }

        static void Record(const CLodStructuralReadbackFrameData& frame,
            org::PassRecordContext& recording) {
            for (const auto& copy : frame.copies) recording.Commands().CopyBufferRegion(
                recording.Resolve(copy.destination).GetHandle(), 0u,
                recording.Resolve(copy.source).GetHandle(), 0u, copy.bytes);
        }

        void ShutdownPass() { CancelArmedSnapshot(); }

    private:
        static void DeclareCopy(org::PassBuilder& builder,
            const std::shared_ptr<org::Buffer>& staging,
            const std::shared_ptr<org::Buffer>& source) {
            if (!source || !staging) return;
            builder.WithCopySource(source);
            builder.WithCopyDest(staging);
        }

        static void CaptureCopy(CLodStructuralReadbackFrameData& frame,
            const org::PassPrepareContext& preparation,
            const std::shared_ptr<org::Buffer>& staging,
            const std::shared_ptr<org::Buffer>& source) {
            if (!source || !staging) return;
            uint64_t bytes = 0;
            if (source->TryGetBufferByteSize(bytes) && bytes > 0) {
                frame.copies.push_back({
                    preparation.CaptureResource(staging->GetGlobalResourceID()),
                    preparation.CaptureResource(source->GetGlobalResourceID()), bytes});
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


}

CLodStreamingSystem::CLodStreamingSystem() {
    auto tagBufferUsage = [](const std::shared_ptr<org::Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            org::memory::SetResourceUsageHint(*buffer, std::string(usage));
        }
    };

    m_streamingNonResidentBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), ~0u);
    m_streamingNonResidentBitsDirtyWordFlags.assign(m_streamingNonResidentBitsCpu.size(), 0u);
    m_streamingActiveGroupsBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_streamingPinnedGroupsBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_streamingResidencyInitializedBitsCpu.assign(CLodBitsetWordCount(m_streamingStorageGroupCapacity), 0u);
    m_groupLastUsedTick.assign(m_streamingStorageGroupCapacity, 0u);
    m_recentlyUsedGroupTrackedCpu.assign(m_streamingStorageGroupCapacity, 0u);
    m_streamingRequestStateByGroup.assign(m_streamingStorageGroupCapacity, StreamingRequestState::None);
    m_pendingLoadPriorityByGroup.assign(m_streamingStorageGroupCapacity, 0u);
    m_pendingStreamingRequestHeapIndexByGroup.assign(m_streamingStorageGroupCapacity, UINT32_MAX);
    m_pendingStreamingRequestGenerationByGroup.assign(m_streamingStorageGroupCapacity, 0u);
    m_readyStreamingCompletionRetryQueuedByGroup.assign(
        m_streamingStorageGroupCapacity, 0u);
    m_readyStreamingCompletionPageCreditWaitQueuedByGroup.assign(
        m_streamingStorageGroupCapacity, 0u);
    m_readyStreamingCompletionWaitPageByGroup.assign(
        m_streamingStorageGroupCapacity, UINT32_MAX);
    m_readyStreamingCompletionWaitKeyByGroup.assign(
        m_streamingStorageGroupCapacity, kInvalidCLodMeshPageKey);
    m_readyStreamingCompletionWaitGenerationByGroup.assign(
        m_streamingStorageGroupCapacity, 0u);
    m_readyStreamingCompletionWaitParentByGroup.assign(
        m_streamingStorageGroupCapacity, UINT32_MAX);
    m_readyStreamingCompletionWaitParentGenerationByGroup.assign(
        m_streamingStorageGroupCapacity, 0u);
    m_streamingDiagnosticsByGroup.resize(m_streamingStorageGroupCapacity);
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();

    try {
        auto getFramesInFlight = SettingsManager::GetInstance().getSettingGetter<uint8_t>("numFramesInFlight");
        // Shadow-page dependencies are transient: unlike load requests, a
        // dropped frame may never be regenerated after its page becomes
        // cache-valid. Three slots saturated during normal scene loading and
        // caused permanently stale VSM pages, while eight kept every tested
        // feedback frame lossless.
        m_streamingReadbackRingSize = std::max<uint32_t>(getFramesInFlight(), 8u);
    }
    catch (...) {
        m_streamingReadbackRingSize = 8u;
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

    CreateResidencyStorage(m_streamingStorageGroupCapacity);

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
        m_directStorageLaunchFencePtr = std::make_shared<rhi::TimelinePtr>();
        result = device.CreateTimeline(*m_directStorageLaunchFencePtr, 0, "CLodDirectStorageLaunchFence");
        if (result == rhi::Result::Ok && *m_directStorageLaunchFencePtr) {
            m_directStorageLaunchFenceHandle = m_directStorageLaunchFencePtr->Get();
        }
    }

    const uint64_t counterStagingBytes = sizeof(uint32_t);
    const uint64_t requestsStagingBytes = static_cast<uint64_t>(CLodStreamingRequestCapacity) * sizeof(CLodStreamingRequest);
    const uint64_t usedGroupsCounterStagingBytes = sizeof(uint32_t);
    const uint64_t usedGroupsBufferStagingBytes = static_cast<uint64_t>(CLodUsedGroupsCapacity) * sizeof(uint32_t);
    const uint64_t virtualShadowDependencyCountStagingBytes = sizeof(uint32_t);
    const uint64_t virtualShadowDependenciesStagingBytes =
        static_cast<uint64_t>(CLodVirtualShadowPredictedPageListCapacity()) *
        sizeof(CLodVirtualShadowPredictedPage);
    m_readbackStagingSlots.resize(m_streamingReadbackRingSize);
    for (uint32_t i = 0; i < m_streamingReadbackRingSize; ++i) {
        auto& slot = m_readbackStagingSlots[i];
        slot.counterStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, counterStagingBytes);
        slot.counterStaging->SetName(("CLodReadbackCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.counterStaging, "Cluster LOD streaming readback");
        slot.requestsStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, requestsStagingBytes);
        slot.requestsStaging->SetName(("CLodReadbackRequests_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.requestsStaging, "Cluster LOD streaming readback");
        slot.usedGroupsCounterStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, usedGroupsCounterStagingBytes);
        slot.usedGroupsCounterStaging->SetName(("CLodReadbackUsedGroupsCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.usedGroupsCounterStaging, "Cluster LOD streaming readback");
        slot.usedGroupsBufferStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, usedGroupsBufferStagingBytes);
        slot.usedGroupsBufferStaging->SetName(("CLodReadbackUsedGroupsBuffer_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.usedGroupsBufferStaging, "Cluster LOD streaming readback");
        const uint64_t sourceGroupMismatchCounterStagingBytes = sizeof(uint32_t);
        const uint64_t sourceGroupMismatchDetailsStagingBytes =
            static_cast<uint64_t>(CLodSourceGroupMismatchDetailCapacity) * sizeof(CLodSourceGroupMismatchDetail);
        slot.sourceGroupMismatchCounterStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, sourceGroupMismatchCounterStagingBytes);
        slot.sourceGroupMismatchCounterStaging->SetName(("CLodReadbackSourceGroupMismatchCounter_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.sourceGroupMismatchCounterStaging, "Cluster LOD diagnostics readback");
        slot.sourceGroupMismatchDetailsStaging = org::Buffer::CreateShared(rhi::HeapType::Readback, sourceGroupMismatchDetailsStagingBytes);
        slot.sourceGroupMismatchDetailsStaging->SetName(("CLodReadbackSourceGroupMismatchDetails_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.sourceGroupMismatchDetailsStaging, "Cluster LOD diagnostics readback");
        slot.virtualShadowDependencyCountStaging =
            org::Buffer::CreateShared(rhi::HeapType::Readback, virtualShadowDependencyCountStagingBytes);
        slot.virtualShadowDependencyCountStaging->SetName(
            ("CLodReadbackVsmFallbackDependencyCount_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.virtualShadowDependencyCountStaging, "Cluster LOD virtual shadow dependency readback");
        slot.virtualShadowDependenciesStaging =
            org::Buffer::CreateShared(rhi::HeapType::Readback, virtualShadowDependenciesStagingBytes);
        slot.virtualShadowDependenciesStaging->SetName(
            ("CLodReadbackVsmFallbackDependencies_" + std::to_string(i)).c_str());
        tagBufferUsage(slot.virtualShadowDependenciesStaging, "Cluster LOD virtual shadow dependency readback");
    }

    StartStreamingService();
}

CLodStreamingSystem::~CLodStreamingSystem() {
    {
        std::lock_guard lock(m_streamingWakeState->mutex);
        m_streamingWakeState->owner = nullptr;
    }
    Shutdown();
    DestroyParallelSortResources();
}

void CLodStreamingSystem::ShutdownGraphResources() {
    StopStreamingService();

    if (ICLodGeometryStorage* meshManager = m_geometryStorage) {
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

void CLodStreamingSystem::QuiesceGraphResourceAccess() {
    StopStreamingService();
    InvalidateVirtualShadowUpgradeUploadMappings();
}

void CLodStreamingSystem::Shutdown() {
    const bool wasAlreadyQuitting = m_streamingServiceStop.load(std::memory_order_acquire);
    if (!wasAlreadyQuitting && m_geometryStorage) {
        if (ICLodGeometryStorage* meshManager = m_geometryStorage) {
            const auto [pendingLaunches, pendingUploads] = meshManager->GetPendingCLodDirectStorageCounts();
            if (pendingLaunches != 0u || pendingUploads != 0u) {
                spdlog::warn(
                    "CLod streaming shutdown with pending DirectStorage work: launches={} uploads={}",
                    pendingLaunches,
                    pendingUploads);
            }
        }
    }
    StopStreamingService();
    WriteStreamingRequestTraceReport();

    // Destruction transfers streaming ownership back to MeshManager/PagePool.
    // A render-graph registry reset does not: the same streaming system remains
    // alive and must retain its resident pages across the graph rebuild.
    ResetStreamingStateForShutdown();
}

void CLodStreamingSystem::OnRegistryReset(org::ResourceRegistry* reg) {
    (void)reg;
    StopStreamingService();
    ClearVirtualShadowUpgradeState();

    // The registry and the alias placements are graph-local, but CLod residency
    // is not. Keep the CPU domain, page ownership, resident groups, LRU and
    // pending disk work intact while releasing only graph-facing backings.
    auto releaseBufferBacking = [](const std::shared_ptr<org::Buffer>& buffer) {
        if (buffer) {
            buffer->Dematerialize();
        }
    };

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

    if (ICLodGeometryStorage* meshManager = m_geometryStorage) {
        ClearStreamingUploadFunction(meshManager);
    }
    // Residency storages are published state rather than graph-local backings
    // and keep their contents. Re-upload the authoritative CPU mirror anyway,
    // and refill any storage whose fill batch may not survive the rebuild.
    for (auto& storage : m_residencyStorages) {
        if (!storage.published) {
            storage.fillQueued = false;
            storage.fillBatchId = 0;
        }
    }
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();
    m_publishedActiveGroupScanCount = 0u;
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
    StopStreamingService();
    ClearVirtualShadowUpgradeState();

    auto releaseBufferBacking = [](const std::shared_ptr<org::Buffer>& buffer) {
        if (buffer) {
            buffer->Dematerialize();
        }
    };

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
    ICLodGeometryStorage* meshManager = m_geometryStorage;

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
    for (CLodPageLRU& lru : m_pageLrus) {
        lru.Clear();
    }
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
    m_readyStreamingCompletionRetryGroups.clear();
    m_readyStreamingCompletionPageCreditWaitGroups.clear();
    m_readyStreamingCompletionPageCreditWaitCursor = 0u;
    m_readyStreamingCompletionBytes = 0u;
    m_peakReadyStreamingCompletionBytes = 0u;
    m_peakReadyStreamingCompletionCount = 0u;
    std::fill(
        m_readyStreamingCompletionRetryQueuedByGroup.begin(),
        m_readyStreamingCompletionRetryQueuedByGroup.end(),
        0u);
    std::fill(
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup.begin(),
        m_readyStreamingCompletionPageCreditWaitQueuedByGroup.end(),
        0u);
    std::fill(
        m_readyStreamingCompletionWaitPageByGroup.begin(),
        m_readyStreamingCompletionWaitPageByGroup.end(),
        UINT32_MAX);
    std::fill(
        m_readyStreamingCompletionWaitKeyByGroup.begin(),
        m_readyStreamingCompletionWaitKeyByGroup.end(),
        kInvalidCLodMeshPageKey);
    m_readyStreamingCompletionWaitersByPage.clear();
    m_readyStreamingCompletionWaitersByParent.clear();
    std::fill(
        m_readyStreamingCompletionWaitParentByGroup.begin(),
        m_readyStreamingCompletionWaitParentByGroup.end(),
        UINT32_MAX);
    m_pendingResidencyCommitGroups.clear();
    m_groupsUsingPinnedStorage.clear();
    m_usedGroupsWordsCpu.clear();
    m_recentlyUsedGroupsCpu.clear();
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
    std::fill(
        m_recentlyUsedGroupTrackedCpu.begin(),
        m_recentlyUsedGroupTrackedCpu.end(),
        0u);
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
    for (auto& storage : m_residencyStorages) {
        if (!storage.published) {
            storage.fillQueued = false;
            storage.fillBatchId = 0;
        }
    }
    MarkStreamingNonResidentBitsDirtyAll();
    MarkStreamingActiveGroupsBitsDirty();
    m_publishedActiveGroupScanCount = 0u;
    m_activeGroupsSnapshotQueue.Reset();
    m_retainedActiveGroupsSnapshot.reset();
    m_streamingServicePublishedGeneration = 0;
    m_streamingDomainEventScratch.clear();
    m_childGroupsScratch.clear();
    m_lastStreamingDomainEventGeneration = 0;
    m_streamingDomainFullResetPending = true;

    // Discard any stale decoded readback data from the scheduler drain.
    m_decodedReadbackBatch.clear();
    m_decodedUsedGroupsBatch.clear();

    // Clear in-flight flags so the scheduler drain doesn't process stale readback data.
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

void CLodStreamingSystem::Initialize(org::RenderGraph& rg) {
    StopStreamingService();
    ICLodGeometryStorage* meshManager = m_geometryStorage;
    if (meshManager != nullptr) {
        ClearStreamingUploadFunction(meshManager);
    }
    // Create a dedicated copy queue for async CLod streaming uploads.
    m_uploadQueueSlot = rg.CreateQueue(
        org::QueueKind::Copy,
        "CLodAsyncUpload",
        org::QueueAutoAssignmentPolicy::ManualOnly);

    if (!m_uploadStream) {
        m_uploadStream = std::make_unique<CLodUploadStream>();
    }

    EnsureParallelSortResources();
    if (meshManager != nullptr && m_pageLruInitialized) {
        InstallStreamingUploadFunction(meshManager);
    }

    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    ++m_uploadBatchGeneration;
    StartStreamingService();
}

void CLodStreamingSystem::StartStreamingService() {
    if (m_streamingTaskScope.Valid()) return;
    m_streamingServiceStop.store(false, std::memory_order_release);
    m_streamingLastProcessedFence = 0;
    m_streamingObservedServiceEpoch = 0;
    m_streamingDrainScheduled.store(false, std::memory_order_release);
    auto& scheduler = br::TaskSchedulerManager::GetInstance();
    if (!scheduler.IsInitialized()) {
        spdlog::error("CLod streaming cannot start before the unified task scheduler");
        m_streamingServiceStop.store(true, std::memory_order_release);
        return;
    }
    m_streamingTaskScope = scheduler.CreateScope("CLodStreaming");
    ScheduleStreamingDrain();
}

void CLodStreamingSystem::StopStreamingService() {
    m_streamingServiceStop.store(true, std::memory_order_release);
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    if (m_streamingTaskScope.Valid()) m_streamingTaskScope.CancelAndWait();
    m_streamingTaskScope = {};
    m_streamingDrainScheduled.store(false, std::memory_order_release);
}

void CLodStreamingSystem::ClearStreamingUploadFunction(ICLodGeometryStorage* meshManager) {
    if (meshManager == nullptr) {
        return;
    }

    meshManager->SetCLodStreamingWakeFunction({});
    {
        std::lock_guard<std::mutex> lock(m_streamingWakeState->mutex);
        m_streamingWakeState->owner = nullptr;
    }
    meshManager->SetCLodStreamingUploadFunction({});
    if (PagePool* pool = meshManager->GetCLodPagePool()) {
        pool->SetUploadFunction({});
    }
}

void CLodStreamingSystem::InstallStreamingUploadFunction(ICLodGeometryStorage* meshManager) {
    if (meshManager == nullptr || m_uploadStream == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_streamingWakeState->mutex);
        m_streamingWakeState->owner = this;
    }
    meshManager->SetCLodStreamingWakeFunction(
        [wakeState = m_streamingWakeState]() {
            std::lock_guard<std::mutex> lock(wakeState->mutex);
            if (wakeState->owner != nullptr) {
                wakeState->owner->RequestStreamingFrameWork();
            }
        });

    PagePool* pool = meshManager->GetCLodPagePool();
    if (pool == nullptr) {
        meshManager->SetCLodStreamingWakeFunction({});
        std::lock_guard<std::mutex> lock(m_streamingWakeState->mutex);
        m_streamingWakeState->owner = nullptr;
        return;
    }

    auto pagePoolUploadFn = [stream = m_uploadStream.get()](
        const void* data, size_t size,
        org::runtime::UploadTarget target, size_t offset) {
        stream->UploadPageData(
            data,
            size,
            std::move(target),
            offset);
    };
    auto meshUploadFn = [stream = m_uploadStream.get()](
        const void* data, size_t size,
        org::runtime::UploadTarget target, size_t offset) {
        if (target.kind == org::runtime::UploadTarget::Kind::PinnedShared) {
            if (auto dynamicBuffer = std::dynamic_pointer_cast<org::DynamicBuffer>(target.pinned)) {
                dynamicBuffer->RetainExternalUpload(data, size, offset);
            }
        }
        stream->UploadData(data, size, std::move(target), offset);
    };
    meshManager->SetCLodStreamingUploadFunction(
        std::move(meshUploadFn));
    // MeshManager forwards its generic upload callback into PagePool. Replace
    // that forwarding callback last so page payloads use the bulk staging path
    // without paying DynamicBuffer shadow-retention checks per page.
    pool->SetUploadFunction(std::move(pagePoolUploadFn));
}

void CLodStreamingSystem::RequestStreamingFrameWork() {
    ZoneScopedN("CLodStreamingSystem::RequestStreamingFrameWork");
    m_streamingServiceEpoch.fetch_add(1u, std::memory_order_release);
    ScheduleStreamingDrain();
}

void CLodStreamingSystem::PublishStreamingFrameWorkForFrame() {
    ZoneScopedN("CLodStreamingSystem::PublishStreamingFrameWorkForFrame");
    const auto upgradeJobs = m_virtualShadowUpgradeQueue.ReadCounters();
    BT_PLOT("CLodStreaming.VSMUpgrade.AcceptedJobs", static_cast<int64_t>(upgradeJobs.accepted));
    BT_PLOT("CLodStreaming.VSMUpgrade.PendingJobs", static_cast<int64_t>(upgradeJobs.pending));
    BT_PLOT("CLodStreaming.VSMUpgrade.ReservedJobs", static_cast<int64_t>(upgradeJobs.reserved));
    BT_PLOT("CLodStreaming.VSMUpgrade.SubmittedJobs", static_cast<int64_t>(upgradeJobs.submitted));
    BT_PLOT("CLodStreaming.VSMUpgrade.ReturnedJobs", static_cast<int64_t>(upgradeJobs.returned));
    BT_PLOT("CLodStreaming.VSMUpgrade.DiscardedJobs", static_cast<int64_t>(upgradeJobs.discarded));
    CLodActiveGroupsSnapshot newest;
    bool received = false;
    m_activeGroupsSnapshotQueue.Drain([&](CLodActiveGroupsSnapshot&& snapshot) {
        newest = std::move(snapshot);
        received = true;
    });
    if (received) {
        m_publishedActiveGroupScanCount = newest.activeGroupScanCount;
        m_streamingServicePublishedGeneration = newest.generation;
    }
}

void CLodStreamingSystem::PublishActiveGroupSnapshot() {
    if (m_retainedActiveGroupsSnapshot) {
        if (!m_activeGroupsSnapshotQueue.TryPush(std::move(*m_retainedActiveGroupsSnapshot))) {
            if (m_streamingActiveGroupsBitsUploadPending) {
                const uint32_t capacity =
                    m_streamingGpuStorageGroupCapacity.load(std::memory_order_acquire);
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
    const auto serviceBegin = std::chrono::steady_clock::now();
    uint64_t domainUs = 0, readbackUs = 0, requestsUs = 0;
    uint64_t shadowUs = 0, residencyUploadUs = 0, finalizeUs = 0;
    const auto measure = [](uint64_t& elapsed, auto&& operation) {
        const auto begin = std::chrono::steady_clock::now();
        operation();
        elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin).count());
    };
    if (NvPerfCaptureSuppressesCLodService()) {
        return;
    }

    if (!PublishRetainedUploadBatch()) {
        return; // lossless backpressure: do not accept more completions yet
    }
    ObserveUploadBatchTickets();
    if (m_retainedUploadBatch) return;
    ICLodGeometryStorage* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::GetMeshManager");
        meshManager = m_geometryStorage;
    }
    if (m_uploadStream == nullptr) {
        return;
    }
    // Geometry cuts wait for a published bitset covering their group tables;
    // grow to whatever capacity they have asked for.
    if (meshManager != nullptr) {
        if (const auto storages = meshManager->GetCLodResidencyStorages()) {
            EnsureStreamingStorageCapacity(storages->RequestedCapacity());
        }
    }

    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::ProcessStreamingDomainEvents");
        measure(domainUs, [this] { ProcessStreamingDomainEvents(); });
    }
    if (meshManager != nullptr) {
        {
            ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::PollCompletedReadbackSlots");
            measure(readbackUs, [this] { PollCompletedReadbackSlots(); });
        }
        {
            ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::ProcessStreamingRequestsBudgeted");
            measure(requestsUs, [this] { ProcessStreamingRequestsBudgeted(); });
        }
    }
    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::PublishVirtualShadowUpgradeUpload");
        measure(shadowUs, [this] { PublishVirtualShadowUpgradeUpload(); });
    }
    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::QueuePendingNonResidentBitsUpload");
        measure(residencyUploadUs, [this] { QueuePendingNonResidentBitsUpload(); });
    }
    measure(finalizeUs, [this] {
        SealStreamingUploadBatch();
        PublishActiveGroupSnapshot();
    });

    {
        ZoneScopedN("CLodStreamingSystem::RunStreamingServiceWork::PublishTelemetry");
        TracyPlot("CLodStreaming.Service.PendingDecodedGroups", static_cast<int64_t>(m_readbackBatchScratch.size()));
        TracyPlot("CLodStreaming.Service.PendingCpuRequests", static_cast<int64_t>(m_pendingStreamingRequestCount));
    }

    const auto totalUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - serviceBegin).count());
    const uint64_t nowMs = ClodDiagNowMs();
    if (totalUs > 4000u && nowMs - m_streamingLastLongSliceDiagnosticMs >= 5000u) {
        m_streamingLastLongSliceDiagnosticMs = nowMs;
        spdlog::warn(
            "CLod scheduler slice {}us: domain={} readback={} requests={} shadow={} residency_upload={} finalize={} pending_cpu={}",
            totalUs, domainUs, readbackUs, requestsUs, shadowUs, residencyUploadUs, finalizeUs,
            m_pendingStreamingRequestCount);
    }

}

void CLodStreamingSystem::GatherStructuralPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses) {
    if (!m_nonResidentBitsResolver) {
        m_nonResidentBitsResolver = std::make_shared<PublishedStateResourceResolver>(
            br::render::PublishedStateSource::ProcessSource(),
            br::render::PublishedResourceKey{
                br::render::PublishedFragmentKind::Geometry,
                br::render::PublishedResourceUsage::ShaderResource, 0, 0,
                br::render::kCLodNonResidentBitsCatalogVariant },
            m_initialResidencyStorage);
    }
    rg.RegisterResolver(Builtin::CLod::StreamingNonResidentBits, m_nonResidentBitsResolver);
    if (m_geometryStorage) {
        if (const auto storages = m_geometryStorage->GetCLodResidencyStorages()) storages->AttachProducer();
    }
    rg.RegisterResource(Builtin::CLod::StreamingLoadRequestKeys, m_streamingLoadRequestKeys);
    rg.RegisterResource(Builtin::CLod::StreamingLoadRequests, m_streamingLoadRequests);
    rg.RegisterResource(Builtin::CLod::StreamingLoadCounter, m_streamingLoadCounter);
    rg.RegisterResource(Builtin::CLod::StreamingRuntimeState, m_streamingRuntimeState);
    rg.RegisterResource(Builtin::CLod::StreamingTouchedGroupsCounter, m_usedGroupsCounter);
    rg.RegisterResource(Builtin::CLod::StreamingTouchedGroups, m_usedGroupsBuffer);

    auto asyncUploadInsertPoint =
        org::RenderGraph::ExternalInsertPoint::After("EvaluateMaterialGroupsPass");
    asyncUploadInsertPoint.AlsoBefore("GTAOFilterPass");
    asyncUploadInsertPoint.AlsoBefore("DeferredShadingPass");
    asyncUploadInsertPoint.keepExtensionOrder = false;
    outPasses.push_back(
        org::RenderGraph::ExternalPassDesc::Copy(
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
                [this](CLodAsyncUploadSnapshot& snapshot) -> org::PassReturn {
                    org::PassReturn result{};
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
                }))
            .At(std::move(asyncUploadInsertPoint))
            .PreferQueue(org::QueueKind::Copy)
            .PinToQueue(m_uploadQueueSlot));

	auto streamingBeginPass = std::make_shared<CLodStreamingBeginFramePass>(
		[]() -> org::UploadInstance* { return nullptr; },
		m_streamingLoadCounter,
        m_streamingLoadRequestKeys,
		m_usedGroupsCounter,
        m_sourceGroupMismatchCounter,
		m_streamingRuntimeState,
		[](std::vector<uint32_t>& outBits, uint32_t& outFirstWord, org::UploadInstance*) {
            // Non-resident data is sealed by the single streaming writer into
            // the same ticketed batch as page payload and page-map changes.
            outBits.clear();
            outFirstWord = 0u;
            return false;
        },
		[this](const UpdateContext& context) {
            // Groups past the capacity of the bitset this frame binds read as
            // nonresident; its published geometry never references them.
            return std::min(m_publishedActiveGroupScanCount, BoundResidencyCapacity(context));
		},
        [this]() {
            PublishStreamingFrameWorkForFrame();
        },
        [this]() {
            RequestStreamingFrameWork();
        });

    auto streamingBeginPassDesc = org::RenderGraph::ExternalPassDesc::Compute(
        "CLod::StreamingBeginFramePass",
        streamingBeginPass);
    // Keep the CLod front-end behind the visibility/depth clear so the graph
    // cannot legally sink ClearVisibilityBufferPass after CLod rasterization.
    auto streamingBeginInsertPoint =
        org::RenderGraph::ExternalInsertPoint::After("ClearVisibilityBufferPass");
    streamingBeginInsertPoint.keepExtensionOrder = false;
    streamingBeginPassDesc.At(std::move(streamingBeginInsertPoint));
    outPasses.push_back(std::move(streamingBeginPassDesc));
}

void CLodStreamingSystem::GatherStructuralTailPasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses) {
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
            org::RenderGraph::ExternalPassDesc::Compute(
                "CLod::StreamingFeedbackSort",
                feedbackSortPass)
                .At(org::RenderGraph::ExternalInsertPoint::After("PresentationReadyPass"))
                .PreferQueue(org::QueueKind::Graphics));
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
                m_virtualShadowFeedbackLossPending = true;
                return false;
            }
            if (m_virtualShadowFeedbackLossPending) {
                m_virtualShadowFeedbackLossPending = false;
                ++m_virtualShadowFeedbackRecoveryRequests;
                g_clodVirtualShadowFeedbackRecoveryRequested.store(
                    true,
                    std::memory_order_release);
                spdlog::warn(
                    "CLOD VSM streaming feedback recovered after a dropped readback frame; scheduling a conservative cached-page refresh.");
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
            snapshot.inputs.virtualShadowDependencyCountSource =
                m_virtualShadowFallbackDependencyCountBuffer;
            snapshot.inputs.virtualShadowDependenciesSource =
                m_virtualShadowFallbackDependenciesBuffer;
            snapshot.counterStaging = slot.counterStaging;
            snapshot.requestsStaging = slot.requestsStaging;
            snapshot.usedGroupsCounterStaging = slot.usedGroupsCounterStaging;
            snapshot.usedGroupsBufferStaging = slot.usedGroupsBufferStaging;
            snapshot.sourceGroupMismatchCounterStaging = slot.sourceGroupMismatchCounterStaging;
            snapshot.sourceGroupMismatchDetailsStaging = slot.sourceGroupMismatchDetailsStaging;
            snapshot.virtualShadowDependencyCountStaging =
                slot.virtualShadowDependencyCountStaging;
            snapshot.virtualShadowDependenciesStaging =
                slot.virtualShadowDependenciesStaging;
            snapshot.selectedSlot = selectedSlot;
            return true;
        },
        [this](uint32_t selectedSlot) -> org::PassReturn {
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
        [wakeState = m_streamingWakeState](uint32_t selectedSlot) {
            std::lock_guard lock(wakeState->mutex);
            auto* owner = wakeState->owner;
            if (!owner || selectedSlot >= owner->m_readbackStagingSlots.size()) return;
            auto expected = ReadbackStagingSlot::State::Recording;
            if (owner->m_readbackStagingSlots[selectedSlot].state.compare_exchange_strong(
                expected, ReadbackStagingSlot::State::Free,
                std::memory_order_acq_rel, std::memory_order_acquire)) return;
            expected = ReadbackStagingSlot::State::Submitted;
            owner->m_readbackStagingSlots[selectedSlot].state.compare_exchange_strong(
                expected, ReadbackStagingSlot::State::Free,
                std::memory_order_acq_rel, std::memory_order_acquire);
        });

    outPasses.push_back(
        org::RenderGraph::ExternalPassDesc::Copy(
            "CLod::StreamingReadbackCopy",
            readbackPass)
            .At(org::RenderGraph::ExternalInsertPoint::After(
                hasStreamingFeedbackSort ? "CLod::StreamingFeedbackSort" : "PresentationReadyPass"))
            .PreferQueue(org::QueueKind::Graphics));

    CLodDirectStorageLaunchInputs launchInputs{};
    if (m_geometryStorage) {
        if (ICLodGeometryStorage* meshManager = m_geometryStorage) {
            if (PagePool* pool = meshManager->GetCLodPagePool()) {
                auto slabGroup = pool->GetSlabResourceGroup();
                if (auto pageTable = pool->GetPageTableBuffer()) {
                    slabGroup->AddResource(pageTable);
                }
                launchInputs.targetSlabResolver = std::make_unique<ResourceGroupResolver>(slabGroup);
            }
        }
    }
    launchInputs.reserveLaunch = [wakeState = m_streamingWakeState]() -> std::shared_ptr<const org::PreparedLifecycleEffect> {
        std::shared_ptr<org::runtime::ExternalSignalReservation> reservation;
        {
            std::lock_guard lock(wakeState->mutex);
            auto* owner = wakeState->owner;
            if (!owner || !owner->m_directStorageLaunchFenceHandle.IsValid() ||
                owner->m_directStorageArmedLaunchFenceValue.load(std::memory_order_acquire) != 0u ||
                !owner->m_directStorageLaunchRequested.load(std::memory_order_acquire)) return {};
            const auto value = owner->m_directStorageLaunchFenceCounter.fetch_add(1, std::memory_order_relaxed) + 1u;
            reservation = std::make_shared<org::runtime::ExternalSignalReservation>(
                owner->m_directStorageLaunchFencePtr, value, [wakeState, value] {
                    std::lock_guard cancelLock(wakeState->mutex);
                    auto* current = wakeState->owner;
                    if (!current) return;
                    auto expected = value;
                    if (current->m_directStorageArmedLaunchFenceValue.compare_exchange_strong(
                        expected, 0u, std::memory_order_acq_rel)) {
                        current->m_directStorageLaunchRequested.store(true, std::memory_order_release);
                        current->RequestStreamingFrameWork();
                    }
                });
            if (!owner->m_directStorageLaunchRequested.exchange(false, std::memory_order_acq_rel)) return {};
            owner->m_directStorageArmedLaunchFenceValue.store(value, std::memory_order_release);
        }
        return reservation;
    };

    outPasses.push_back(
        org::RenderGraph::ExternalPassDesc::Copy(
            "CLod::DirectStorageLaunch",
            std::make_shared<CLodDirectStorageLaunchPass>(std::move(launchInputs)))
            .At(org::RenderGraph::ExternalInsertPoint::After("PresentationReadyPass"))
            .PreferQueue(org::QueueKind::Graphics));
}

void CLodStreamingSystem::GatherFramePasses(org::RenderGraph& rg, std::vector<org::RenderGraph::ExternalPassDesc>& outPasses) {
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

    auto tagBufferUsage = [](const std::shared_ptr<org::Buffer>& buffer, std::string_view usage) {
        if (buffer) {
            org::memory::SetResourceUsageHint(*buffer, std::string(usage));
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
        m_streamingNonResidentBitsUploadPending = false;
        m_streamingNonResidentBitsDirtyBegin = 0u;
        m_streamingNonResidentBitsDirtyEnd = 0u;
        m_streamingNonResidentBitsDirtyWords.clear();
        m_streamingNonResidentBitsDirtyWordCursor = 0u;
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
        if (groupIndex >= m_virtualShadowResidencyGenerationByGroup.size()) {
            m_virtualShadowResidencyGenerationByGroup.resize(
                static_cast<size_t>(groupIndex) + 1u,
                1u);
        }
        ++m_virtualShadowResidencyGenerationByGroup[groupIndex];
        if (m_virtualShadowResidencyGenerationByGroup[groupIndex] == 0u) {
            m_virtualShadowResidencyGenerationByGroup[groupIndex] = 1u;
        }
        auto dependencies =
            RemoveVirtualShadowDependencyBucket(groupIndex);
        if (!dependencies.empty()) {
            m_virtualShadowUpgradeStats.clearedDependencies +=
                dependencies.size();
            dependencies.clear();
            if (m_virtualShadowDependencyBucketPool.size() < 256u) {
                m_virtualShadowDependencyBucketPool.push_back(
                    std::move(dependencies));
            }
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

void CLodStreamingSystem::InitializePageLru(ICLodGeometryStorage* meshManager) {
    ZoneScopedN("CLodStreamingSystem::InitializePageLru");

    if (m_pageLruInitialized || !meshManager) return;

    auto* pool = meshManager->GetCLodPagePool();
    if (!pool) return;

    const uint32_t totalPages = pool->GetTotalPageCount();
    if (pool->GetGeneralPageCount() == 0 || totalPages == 0) return;

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
    m_readyStreamingCompletionWaitersByPage.clear();
    m_readyStreamingCompletionWaitersByPage.resize(totalPages);
    m_pageProtectedThisUpdate.assign(totalPages, 0u);

    {
        ZoneScopedN("CLodStreamingSystem::InitializePageLru::PopulateFreePages");
        for (uint32_t classIndex = 0u;
            classIndex < PagePool::GetPageSizeClassCount();
            ++classIndex) {
            const uint32_t classSize = 16u * 1024u << classIndex;
            for (uint32_t page : pool->GetGeneralPageIDs(classSize)) {
                m_pageLrus[classIndex].Insert(page);
            }
        }
    }

    m_pageLruInitialized = true;

    // Route PagePool uploads through the serial-drain-owned CLod upload stream.
    InstallStreamingUploadFunction(meshManager);
    spdlog::info(
        "CLodPageLRU initialized with {} general pages across {} size classes",
        pool->GetGeneralPageCount(),
        PagePool::GetPageSizeClassCount());
}

CLodPageLRU& CLodStreamingSystem::PageLruForPage(uint32_t page) {
    ICLodGeometryStorage* meshManager = m_geometryStorage;
    PagePool* pool = meshManager != nullptr ? meshManager->GetCLodPagePool() : nullptr;
    return m_pageLrus[pool != nullptr ? pool->GetPageSizeClassIndex(page) : 0u];
}

const CLodPageLRU& CLodStreamingSystem::PageLruForPage(uint32_t page) const {
    ICLodGeometryStorage* meshManager = m_geometryStorage;
    PagePool* pool = meshManager != nullptr ? meshManager->GetCLodPagePool() : nullptr;
    return m_pageLrus[pool != nullptr ? pool->GetPageSizeClassIndex(page) : 0u];
}

uint32_t CLodStreamingSystem::TotalPageLruSize() const {
    uint32_t total = 0u;
    for (const CLodPageLRU& lru : m_pageLrus) {
        total += lru.Size();
    }
    return total;
}

void CLodStreamingSystem::EnsurePageTrackingCapacity(ICLodGeometryStorage* meshManager) {
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
        m_readyStreamingCompletionWaitersByPage.resize(totalPages);
        m_pageProtectedThisUpdate.resize(totalPages, 0u);
    }
}

void CLodStreamingSystem::ReleaseOwnedPagesForGroup(uint32_t groupIndex, ICLodGeometryStorage* meshManager) {
    ReleaseGroupResidency(groupIndex, meshManager, false);
}

uint64_t CLodStreamingSystem::StreamingUploadVisibilityDelayTicks() const {
    return static_cast<uint64_t>(m_streamingReadbackRingSize + 2u);
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
    if (m_uploadStream == nullptr) {
        return;
    }

    // Storages still bindable by some published state or in-flight frame. A
    // storage nothing else references can never be read again.
    std::vector<std::pair<std::shared_ptr<org::Buffer>, uint32_t>> liveStorages;
    liveStorages.reserve(m_residencyStorages.size());
    for (auto it = m_residencyStorages.begin(); it != m_residencyStorages.end();) {
        auto buffer = it->buffer.lock();
        if (!buffer) {
            it = m_residencyStorages.erase(it);
            continue;
        }
        if (!it->fillQueued) {
            // The complete mirror, in the same batch stream as every later change,
            // so the storage is exact once this batch completes.
            const uint32_t words = std::min<uint32_t>(
                CLodBitsetWordCount(it->capacity), static_cast<uint32_t>(m_streamingNonResidentBitsCpu.size()));
            if (words != 0u) {
                m_uploadStream->UploadData(
                    m_streamingNonResidentBitsCpu.data(),
                    static_cast<size_t>(words) * sizeof(uint32_t),
                    org::runtime::UploadTarget::FromShared(buffer),
                    0u);
            }
            it->fillQueued = true;
            it->fillBatchId = 0u;
        }
        liveStorages.emplace_back(std::move(buffer), CLodBitsetWordCount(it->capacity));
        ++it;
    }
    if (!m_streamingNonResidentBitsUploadPending) {
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
        for (const auto& [storage, storageWords] : liveStorages) {
            if (firstWord >= storageWords) continue;
            const uint32_t words = std::min<uint32_t>(
                static_cast<uint32_t>(uploadBits.size()), storageWords - firstWord);
            m_uploadStream->UploadData(
                uploadBits.data(),
                static_cast<size_t>(words) * sizeof(uint32_t),
                org::runtime::UploadTarget::FromShared(storage),
                firstWord * sizeof(uint32_t));
        }
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

void CLodStreamingSystem::PrefetchChildGroupLayouts(uint32_t parentGroupIndex, ICLodGeometryStorage* meshManager) {
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
    std::vector<br::render::CLodPrefetchedChildLayout>&& prefetchedLayouts) {
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

void CLodStreamingSystem::EnsureStreamingStorageCapacity(uint32_t requiredGroupCount) {
    if (requiredGroupCount <= m_streamingStorageGroupCapacity) {
        return;
    }

    const uint32_t newCapacity = CLodRoundUpCapacity(requiredGroupCount);
    const uint32_t newWordCount = CLodBitsetWordCount(newCapacity);

    m_streamingNonResidentBitsCpu.resize(newWordCount, ~0u);
    m_streamingNonResidentBitsDirtyWordFlags.resize(newWordCount, 0u);
    m_streamingActiveGroupsBitsCpu.resize(newWordCount, 0u);
    m_streamingPinnedGroupsBitsCpu.resize(newWordCount, 0u);
    m_streamingResidencyInitializedBitsCpu.resize(newWordCount, 0u);
    m_usedGroupsBitsCpu.resize(newWordCount, 0u);
    m_parentGroupByGroup.resize(newCapacity, UINT32_MAX);
    m_virtualShadowDependencyBucketIndexByGroup.resize(newCapacity, -1);
    m_virtualShadowResidencyGenerationByGroup.resize(newCapacity, 1u);
    m_virtualShadowBatchSourceGenerationByGroup.resize(newCapacity, 0u);
    m_virtualShadowBatchSourceChainOffsetByGroup.resize(newCapacity, 0u);
    m_virtualShadowBatchSourceChainCountByGroup.resize(newCapacity, 0u);
    m_groupLastUsedTick.resize(newCapacity, 0u);
    m_recentlyUsedGroupTrackedCpu.resize(newCapacity, 0u);
    m_streamingRequestStateByGroup.resize(newCapacity, StreamingRequestState::None);
    m_pendingLoadPriorityByGroup.resize(newCapacity, 0u);
    m_pendingStreamingRequestHeapIndexByGroup.resize(newCapacity, UINT32_MAX);
    m_pendingStreamingRequestGenerationByGroup.resize(newCapacity, 0u);
    m_waitingForPagesRequestIndexByGroup.resize(newCapacity, UINT32_MAX);
    m_readyStreamingCompletionRetryQueuedByGroup.resize(newCapacity, 0u);
    m_readyStreamingCompletionPageCreditWaitQueuedByGroup.resize(
        newCapacity, 0u);
    m_readyStreamingCompletionWaitPageByGroup.resize(newCapacity, UINT32_MAX);
    m_readyStreamingCompletionWaitKeyByGroup.resize(
        newCapacity, kInvalidCLodMeshPageKey);
    m_readyStreamingCompletionWaitGenerationByGroup.resize(newCapacity, 0u);
    m_readyStreamingCompletionWaitParentByGroup.resize(
        newCapacity, UINT32_MAX);
    m_readyStreamingCompletionWaitParentGenerationByGroup.resize(
        newCapacity, 0u);
    EnsureStreamingDiagnosticsCapacity(newCapacity);
    m_streamingStorageGroupCapacity = newCapacity;
    // Existing storages keep their (still exact) contents; the new one is filled
    // from the grown mirror by the next worker upload.
    CreateResidencyStorage(newCapacity);
    MarkStreamingActiveGroupsBitsDirty();
}

void CLodStreamingSystem::CreateResidencyStorage(uint32_t capacity) {
    // Allocated on the streaming worker, outside any graph: the buffer is not
    // bindable until its filled revision is published through the state graph.
    org::Resource::ScopedECSRegistrationSuppression suppressECS;
    auto buffer = CreateIndexedStructuredBuffer(CLodBitsetWordCount(capacity), sizeof(uint32_t), true);
    buffer->SetName("CLod Streaming NonResident Bits");
    org::memory::SetResourceUsageHint(*buffer, "Cluster LOD streaming");
    if (!m_initialResidencyStorage) {
        m_initialResidencyStorage = buffer;
    }
    m_residencyStorages.push_back(ResidencyStorage{
        .capacity = capacity,
        .retained = buffer,
        .buffer = buffer,
    });
    m_streamingGpuStorageGroupCapacity.store(capacity, std::memory_order_release);
    TracyPlot("CLodStreaming.Storage.Capacity", static_cast<int64_t>(capacity));
}

void CLodStreamingSystem::PublishFilledResidencyStorages(uint64_t completedBatchId) {
    const auto storages = m_geometryStorage ? m_geometryStorage->GetCLodResidencyStorages() : nullptr;
    for (auto& storage : m_residencyStorages) {
        if (storage.published || !storage.fillQueued || storage.fillBatchId != completedBatchId) continue;
        if (!storages || !storage.retained) {
            // Nothing to publish into yet: refill once the directory exists.
            storage.fillQueued = false;
            storage.fillBatchId = 0u;
            continue;
        }
        storage.published = true;
        spdlog::info("CLod streaming: publishing filled residency bitset capacity={}", storage.capacity);
        storages->PublishStorage(storage.capacity, storage.retained);
        // From here the published state holds it for as long as any frame can
        // bind it; the weak reference keeps it written until then.
        storage.retained.reset();
    }
}

uint32_t CLodStreamingSystem::BoundResidencyCapacity(const UpdateContext& context) const {
    if (!m_nonResidentBitsResolver) return 0u;
    const auto resources = m_nonResidentBitsResolver->ResolveFrom(context.publishedRendererState);
    const auto buffer = resources.empty() ? nullptr : std::dynamic_pointer_cast<org::Buffer>(resources.front());
    return buffer ? static_cast<uint32_t>(buffer->GetSize() / sizeof(uint32_t)) * 32u : 0u;
}

void CLodStreamingSystem::InitializeActiveRange(
    ICLodGeometryStorage* meshManager,
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

        const br::render::CLodGroupStreamingInfo info = meshManager->GetCLodGroupStreamingInfo(groupIndex);
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

    for (auto& storage : m_residencyStorages) {
        if (storage.fillQueued && storage.fillBatchId == 0u) storage.fillBatchId = batch->ticket->batchId;
    }
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
                        RecordStreamingUploadSubmitted(groupIndex);
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
            PublishFilledResidencyStorages(batch->ticket->batchId);
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


void CLodStreamingSystem::ScheduleStreamingDrain() {
    if (m_streamingServiceStop.load(std::memory_order_acquire) || !m_streamingTaskScope.Valid()) return;
    bool expected = false;
    if (!m_streamingDrainScheduled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) return;
    if (!br::TaskSchedulerManager::GetInstance().Submit(
            m_streamingTaskScope,
            br::TaskLane::Streaming,
            br::TaskDomain::General,
            "CLodStreamingDrain",
            [this](const br::TaskContext& context) { StreamingDrainTask(context); })) {
        m_streamingDrainScheduled.store(false, std::memory_order_release);
        spdlog::error("CLod streaming drain submission was rejected");
    }
}

void CLodStreamingSystem::StreamingDrainTask(const br::TaskContext& context) {
        const auto finishDrain = [this]() {
            m_streamingDrainScheduled.store(false, std::memory_order_release);
            if (!m_streamingServiceStop.load(std::memory_order_acquire) &&
                m_streamingServiceEpoch.load(std::memory_order_acquire) != m_streamingObservedServiceEpoch) {
                ScheduleStreamingDrain();
            }
        };
        if (context.StopRequested() || m_streamingServiceStop.load(std::memory_order_acquire)) {
            finishDrain();
            return;
        }
        uint64_t requestedEpoch = m_streamingServiceEpoch.load(std::memory_order_acquire);
        const bool serviceRequested = requestedEpoch != m_streamingObservedServiceEpoch;
        m_streamingObservedServiceEpoch = requestedEpoch;
        uint64_t& lastProcessed = m_streamingLastProcessedFence;

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
            ZoneScopedN("CLodStreamingDrain::PollReadbackFence");
            uint64_t completed = m_streamingReadbackFenceHandle.GetCompletedValue();
            if (completed > lastProcessed) {
                decodeTarget = std::min(completed, submittedTarget);
                readbackReady = true;
            }
        }

        if (readbackReady) {
            // Submitted slots are published by the graph thread with release;
            // this worker is their sole decoder and returns them to Free.
            {
                ZoneScopedN("CLodStreamingWorker::DecodeReadbackSlots");

                std::vector<DecodedStreamingRequest> decodedRequests;
                std::vector<uint32_t> sumSeenGroups;
                std::vector<uint32_t> decodedUsedGroups;
                bool decodedAnyCompletedSlot = false;
                const bool sortedFeedback = m_parallelSortAvailable;
                m_virtualShadowReadbackBatchScratch.clear();

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
                    const uint64_t slotDecodeNs =
                        CLodRequestTraceEnabled()
                        ? CLodRequestTraceNowNs()
                        : 0u;

                    uint32_t virtualShadowDependencyCount = 0u;
                    if (slot.virtualShadowDependencyCountStaging) {
                        auto apiResource =
                            slot.virtualShadowDependencyCountStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            std::memcpy(
                                &virtualShadowDependencyCount,
                                mapped,
                                sizeof(uint32_t));
                            apiResource.Unmap(0, 0);
                        }
                    }
                    virtualShadowDependencyCount = std::min<uint32_t>(
                        virtualShadowDependencyCount,
                        CLodVirtualShadowPredictedPageListCapacity());
                    if (virtualShadowDependencyCount > 0u &&
                        slot.virtualShadowDependenciesStaging) {
                        auto apiResource =
                            slot.virtualShadowDependenciesStaging->GetAPIResource();
                        void* mapped = nullptr;
                        apiResource.Map(&mapped);
                        if (mapped) {
                            const auto* dependencies =
                                static_cast<const CLodVirtualShadowPredictedPage*>(mapped);
                            m_virtualShadowReadbackBatchScratch.insert(
                                m_virtualShadowReadbackBatchScratch.end(),
                                dependencies,
                                dependencies +
                                    virtualShadowDependencyCount);
                            apiResource.Unmap(0, 0);
                        }
                    }

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
                                    m_decodeFirstSeenNsByGroup.resize(
                                        newSize,
                                        0u);
                                }

                                if (m_priorityMode == CLodPriorityMode::Sum || !sortedFeedback) {
                                    if (m_decodeSeenGenerationByGroup[groupIndex] != m_decodeSeenGeneration) {
                                        m_decodeSeenGenerationByGroup[groupIndex] = m_decodeSeenGeneration;
                                        m_decodePriorityAccumByGroup[groupIndex] = priority;
                                        m_decodeFirstSeenNsByGroup[groupIndex] =
                                            slotDecodeNs;
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
                                    decodedRequests.push_back(
                                        DecodedStreamingRequest{
                                            groupIndex,
                                            priority,
                                            slotDecodeNs});
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
									"CLod source group mismatch detail[{}]: expectedLocal={} foundLocal={} expectedGlobal={} foundGlobal={} metadata={} expectedTemplateMetadata={} actualOwnerMetadata={} expectedMeshIdentity={:08x}{:08x} actualMeshIdentity={:08x}{:08x} groupsBase={} expectedSegment={} expectedPage={} expectedMeshlets=[{}, {}) expectedMap={}:{} actualPageLocalMeshlet={} actualMap={}:{} visibleCluster={} unsortedCluster={} instance={} view={} bucketMeshlet={} bucketCount={}",
                                    i,
                                    detail.expectedGroupLocalIndex,
                                    detail.foundGroupLocalIndex,
                                    detail.expectedGroupGlobalIndex,
                                    detail.foundGroupGlobalIndex,
									detail.clodMeshMetadataIndex,
									detail.expectedTemplateMeshMetadataIndex,
									detail.actualOwnerMeshMetadataIndex,
									detail.expectedMeshIdentityHi,
									detail.expectedMeshIdentityLo,
									detail.actualMeshIdentityHi,
									detail.actualMeshIdentityLo,
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

                RecordVirtualShadowUpgradeDependencies(
                    m_virtualShadowReadbackBatchScratch);

                if (!sumSeenGroups.empty()) {
                    decodedRequests.reserve(decodedRequests.size() + sumSeenGroups.size());
                    for (uint32_t groupIndex : sumSeenGroups) {
                        decodedRequests.push_back(
                            DecodedStreamingRequest{
                                groupIndex,
                                m_decodePriorityAccumByGroup[groupIndex],
                                m_decodeFirstSeenNsByGroup[groupIndex]});
                    }
                    std::sort(
                        decodedRequests.begin(),
                        decodedRequests.end(),
                        [](const auto& a, const auto& b) {
                            return a.priority > b.priority;
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
                    for (const auto& decoded : decodedRequests) {
                        m_decodedReadbackBatch.push_back(decoded);
                    }
                    for (const uint32_t g : decodedUsedGroups) {
                        m_decodedUsedGroupsBatch.push_back(g);
                    }
                }

                if (!decodedRequests.empty() || !decodedUsedGroups.empty() ||
                    !m_virtualShadowReadbackBatchScratch.empty()) {
                    RequestStreamingFrameWork();
                }

                TracyPlot("CLodStreaming.Worker.DecodedRequests", static_cast<int64_t>(decodedRequests.size()));
                TracyPlot("CLodStreaming.Worker.DecodedUsedGroups", static_cast<int64_t>(decodedUsedGroups.size()));
                TracyPlot(
                    "CLodStreaming.Worker.DecodedVsmDependencies",
                    static_cast<int64_t>(
                        m_virtualShadowReadbackBatchScratch.size()));
            }

            lastProcessed = decodeTarget;
        }

        const uint64_t latestEpoch = m_streamingServiceEpoch.load(std::memory_order_acquire);
        if (serviceRequested || latestEpoch != m_streamingObservedServiceEpoch) {
            m_streamingObservedServiceEpoch = latestEpoch;
            ZoneScopedN("CLodStreamingWorker::RunStreamingServiceWork");
            m_streamingServiceRunning.store(true, std::memory_order_release);
            RunStreamingServiceWork();
            m_streamingServiceRunning.store(false, std::memory_order_release);
        }
        finishDrain();
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
    // Preserve the configured admission ceiling while consuming a CPU backlog
    // in small continuations that do not monopolize a OneTBB worker.
    const uint32_t budget = std::min<uint32_t>(m_streamingCpuUploadBudgetRequests, 32u);
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

    ICLodGeometryStorage* meshManager = nullptr;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::GetMeshManager");
        meshManager = m_geometryStorage;
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
            // publisher will request another service tick after the signal.
            // Keep the armed launch set frozen, but continue servicing CPU
            // completions, page retirement, and unrelated uploads.
        }
        else {
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
            CommitPendingResidencyPromotions(meshManager);
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::ProcessDiskStreamingIO");
            {
                ZoneScopedN("CLodStreamingWorker::ProcessDiskStreamingIO");
                meshManager->ProcessCLodDiskStreamingIO();
            }
        }
        WakeReadyPageCreditWaiters(CLodPageCreditRetryBudget());
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::StreamingMaintenance::ApplyDiskStreamingCompletions");
            ApplyDiskStreamingCompletions(meshManager);
        }
        PruneStaleReadyStreamingCompletions(
            std::max<uint32_t>(budget, 256u));
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
            const bool recordCpuTiming = basic_telemetry::Enabled();
            const uint64_t timingStartNs =
                recordCpuTiming ? basic_telemetry::NowNs() : 0u;
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
            if (recordCpuTiming) {
                basic_telemetry::Record(
                    "CLod.Bookkeeping.ProtectReferencedPages.UsedGroups",
                    basic_telemetry::NowNs() - timingStartNs);
            }
        }
        {
            ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::ProtectReferencedPages::RecentlyUsedOwnedGroups");
            const bool recordCpuTiming = basic_telemetry::Enabled();
            const uint64_t timingStartNs =
                recordCpuTiming ? basic_telemetry::NowNs() : 0u;
            const uint64_t protectedUsedWindow = static_cast<uint64_t>(std::max<uint32_t>(m_streamingReadbackRingSize, 1u) + 1u);
            size_t retainedGroupCount = 0u;
            for (const uint32_t groupIndex : m_recentlyUsedGroupsCpu) {
                const bool remainsRecent =
                    groupIndex < m_groupLastUsedTick.size() &&
                    m_groupLastUsedTick[groupIndex] != 0u &&
                    m_streamingDiagnosticTick <=
                        m_groupLastUsedTick[groupIndex] + protectedUsedWindow;
                if (!remainsRecent) {
                    if (groupIndex < m_recentlyUsedGroupTrackedCpu.size()) {
                        m_recentlyUsedGroupTrackedCpu[groupIndex] = 0u;
                    }
                    continue;
                }

                m_recentlyUsedGroupsCpu[retainedGroupCount++] = groupIndex;
                const uint32_t protectedWord = BitWordAddress(groupIndex);
                if (protectedWord < m_protectedGroupsBitsScratch.size() &&
                    (m_protectedGroupsBitsScratch[protectedWord] &
                        BitMask(groupIndex)) != 0u) {
                    continue;
                }
                if (m_groupOwnedPages.contains(groupIndex)) {
                    ProtectGroupAndAncestors(groupIndex);
                }
            }
            m_recentlyUsedGroupsCpu.resize(retainedGroupCount);
            if (recordCpuTiming) {
                basic_telemetry::Record(
                    "CLod.Bookkeeping.ProtectReferencedPages.RecentlyUsedOwnedGroups",
                    basic_telemetry::NowNs() - timingStartNs);
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
    uint32_t admissionBudget = budget;
    if (meshManager != nullptr) {
        const auto admissionStats = meshManager->GetCLodStreamingDebugStats();
        m_streamingIoAdmissionDepth = std::max<uint32_t>(
            admissionStats.ioAdmissionTarget, 1u);
        m_streamingIoWorkerCount = admissionStats.ioWorkerCount;
        m_streamingIoTaskBatchSize = admissionStats.ioTaskBatchSize;
        admissionBudget =
            admissionStats.queuedOrInFlightGroups < m_streamingIoAdmissionDepth
            ? std::min<uint32_t>(
                budget,
                m_streamingIoAdmissionDepth -
                    admissionStats.queuedOrInFlightGroups)
            : 0u;
        const uint32_t stagedPayloadGroups =
            static_cast<uint32_t>(
                m_readyStreamingCompletionsByGroup.size());
        const uint32_t outstandingPayloadCredits =
            admissionStats.queuedOrInFlightGroups +
            stagedPayloadGroups;
        const uint32_t stagedPayloadLimit =
            CLodStagedPayloadGroupLimit();
        admissionBudget =
            outstandingPayloadCredits < stagedPayloadLimit
            ? std::min<uint32_t>(
                  admissionBudget,
                  stagedPayloadLimit - outstandingPayloadCredits)
            : 0u;
        TracyPlot(
            "CLodStreaming.Service.IoAdmissionBudget",
            static_cast<int64_t>(admissionBudget));
    }

    struct QueuedStreamingCandidate {
        uint32_t groupIndex = 0u;
        br::render::CLodGroupDiskIOBatchRequest request;
    };
    std::vector<QueuedStreamingCandidate> diskIoBatch;
    diskIoBatch.reserve(admissionBudget);

    uint32_t processed = 0;
    {
        ZoneScopedN("CLodStreamingSystem::ProcessStreamingRequestsBudgeted::SelectAndPrepareRequests");
        {
            ZoneScopedN("CLodStreamingWorker::SelectAndPrepareRequests");
            while (processed < admissionBudget && !m_pendingStreamingRequests.empty()) {
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
                const bool allocatePagesAfterCpuRead =
                    !IsGroupPinned(groupIndex);
                if (!allocatePagesAfterCpuRead &&
                    paIt == m_preAllocatedPagesByGroup.end()) {
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
                candidate.request.deferCpuPayloadCopy =
                    allocatePagesAfterCpuRead;
                if (!allocatePagesAfterCpuRead) {
                    candidate.request.segmentNeedsFetch =
                        paIt->second.segmentNeedsFetch;
                    candidate.request.preAllocatedPages =
                        paIt->second.pagesBySegment;
                }
                candidate.request.priority = priority;
                if (prefetchedLayout != nullptr && prefetchedLayout->IsValid()) {
                    candidate.request.prefetchedLayout = *prefetchedLayout;
                }
                if (!allocatePagesAfterCpuRead &&
                    meshManager->IsCLodStreamingDirectStorageEnabled()) {
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

            std::vector<br::render::CLodGroupDiskIOBatchRequest> batchRequests;
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
