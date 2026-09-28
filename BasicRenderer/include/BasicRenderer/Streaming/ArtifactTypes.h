#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include <array>

#include <BasicRenderer/Streaming/TaskScheduler.h>
#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/ArtifactIdentity.h>
#include <ORGModuleServices/Async/ArtifactSnapshot.h>
#include <ORGModuleServices/Async/ArtifactBuild.h>
#include <ORGModuleServices/Async/GraphTrace.h>

namespace org { struct TrackedUploadTicket; }

namespace br::render {

enum class ArtifactKind : std::uint16_t {
    Generic,
    TextureBinding,
    Material,
    MaterialTable,
    MaterialUsageBatch,
    Mesh,
    MeshTable,
    GeometryBufferState,
    DrawRecordPage,
    ActiveDrawList,
    ViewLifetime,
    IndirectWorkload,
    StaticTransaction,
    StaticScenePage,
    StaticScene,
    TerrainState,
    BufferVersion,
    FrameManifest,
    StaticGroup,
    StaticTemplate,
    StaticTemplateBatch,
    StaticVisibility,
    StaticAsset,
    StaticMaterialVariant,
    StaticShaderVariant,
    StaticVariant,
    TextureImageTable,
    GrassCell,
    GrassShard,
    GrassScratch,
    GrassScene,
    GeometryResidency,
    ViewFamily,
    PoseState,
    LightTable,
    GeometryCoverageGate,
    TextureDisplayGate,
    CLodResidencyStorage,
    CLodResidencyCapacityGate,
};
inline constexpr std::size_t kArtifactKindCount =
    static_cast<std::size_t>(ArtifactKind::CLodResidencyCapacityGate) + 1u;

using ArtifactAddress = org::async::ArtifactAddress<ArtifactKind>;
using ArtifactVersionID = org::async::ArtifactVersionID<ArtifactKind>;
using ArtifactVersionHandle = org::async::ArtifactVersionHandle<ArtifactKind>;
using ArtifactRequirement = org::async::ArtifactRequirement<ArtifactKind>;
using ArtifactKey = ArtifactAddress;
using org::async::ArtifactLease;
using org::async::ArtifactReadiness;
using org::async::ArtifactReachedMilestone;
using org::async::DependencyPolicy;
using org::async::DependencyInvalidationPolicy;

[[nodiscard]] inline ArtifactRequirement Exact(ArtifactVersionID version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return org::async::Exact(version, readiness, policy, alternativeGroup);
}

[[nodiscard]] inline ArtifactRequirement Exact(const ArtifactVersionHandle& handle,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return org::async::Exact(handle, readiness, policy, alternativeGroup);
}

[[nodiscard]] inline ArtifactRequirement Latest(ArtifactAddress address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return org::async::Latest(address, readiness, policy, alternativeGroup);
}

[[nodiscard]] inline ArtifactRequirement LatestAtLeast(ArtifactAddress address,
    std::uint64_t minimumRevision,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return org::async::LatestAtLeast(address, minimumRevision, readiness, policy, alternativeGroup);
}

// Exact-version readiness gate: authorizes the consumer's build once that version
// reaches the milestone, without pinning it. The requester must hold the
// version's handle until the consumer has built; afterwards it may be reclaimed.
[[nodiscard]] inline ArtifactRequirement ReadyGate(ArtifactVersionID version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return org::async::ReadyGate(version, readiness);
}

// Address-level readiness latch. It selects whichever immutable version of the
// address currently satisfies the milestone and deliberately does not retain or
// invalidate on later versions. Use the handle overload when one exact version
// is the gate.
[[nodiscard]] inline ArtifactRequirement ReadyGate(ArtifactAddress address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return org::async::ReadyGate(address, readiness);
}

[[nodiscard]] inline ArtifactRequirement LifetimeHold(ArtifactVersionID version) {
    return org::async::LifetimeHold(version);
}


using HandleRequirement = org::async::HandleRequirement<ArtifactKind>;
using Require = org::async::Require<ArtifactKind>;
using Optional = org::async::Optional<ArtifactKind>;
using FirstReady = org::async::FirstReady<ArtifactKind>;
using AnyReady = org::async::AnyReady<ArtifactKind>;
using DependencyExpression = org::async::DependencyExpression<ArtifactKind>;

using org::async::ArtifactPayload;
using org::async::GpuQueueSubmission;
using org::async::GpuSubmissionSet;

using ArtifactSnapshot = org::async::ArtifactSnapshot<ArtifactKind>;

template <class T>
using ArtifactHandle = org::async::ArtifactHandle<T, ArtifactKind>;

template <class T>
[[nodiscard]] ArtifactHandle<T> MakeArtifactHandle(const ArtifactSnapshot& snapshot) {
    return org::async::MakeArtifactHandle<T>(snapshot);
}

std::shared_ptr<const GpuSubmissionSet> MakeGpuSubmissionSet(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket);

// Renderer-only scheduling defaults; shared contracts do not know this inventory.
struct RendererArtifactScheduling {
    using Lane = TaskLane;
    using Domain = TaskDomain;
    static constexpr Lane initialLane = TaskLane::Streaming;
    static constexpr Lane continuationLane = TaskLane::FrameCritical;
    static constexpr Domain acceptanceDomain = TaskDomain::RendererState;
    static constexpr Domain producerDomain = TaskDomain::General;
    static constexpr std::uint8_t admissionGroup = 1;
};

using ArtifactBuildContext = org::async::ArtifactBuildContext<ArtifactKind>;
using ArtifactRequestResult = org::async::ArtifactRequestResult<ArtifactKind>;
using ArtifactRequest = org::async::ArtifactRequest<ArtifactKind>;
using ArtifactObservation = org::async::ArtifactObservation<ArtifactKind>;
using ArtifactTermination = org::async::ArtifactTermination<ArtifactKind>;
using ArtifactAwaiter = org::async::ArtifactAwaiter<ArtifactKind>;
using ArtifactSuspension = org::async::ArtifactSuspension<ArtifactKind>;
using ArtifactAcceptanceRegistration = org::async::ArtifactAcceptanceRegistration<ArtifactKind, RendererArtifactScheduling>;
using ArtifactBuildResult = org::async::ArtifactBuildResult<ArtifactKind, RendererArtifactScheduling>;
using ArtifactSchedulingPolicy = org::async::ArtifactSchedulingPolicy<RendererArtifactScheduling>;
using ArtifactProducerRegistration = org::async::ArtifactProducerRegistration<ArtifactKind, RendererArtifactScheduling>;
using ArtifactDiagnostic = org::async::ArtifactDiagnostic<ArtifactKind>;
using ArtifactIntent = ArtifactRequest;
using ArtifactProducer = org::async::ArtifactProducer<ArtifactKind, RendererArtifactScheduling>;
using AsyncStateGraphStats = org::async::AsyncStateGraphStats<kArtifactKindCount>;
using org::async::ArtifactRequestStatus;
using org::async::ArtifactSuspensionKind;
using org::async::ArtifactWorkClass;

// Compatibility entry point forwarding to the shared compiled runtime.
[[nodiscard]] std::uint64_t AllocateArtifactSuspensionIdentity() noexcept;

using org::async::AsyncStateGraphTraceDetail;

// Trace records carry only this stable ID and numeric payload on graph workers.
// Human-readable names and detail strings are expanded after capture stops.
enum class AsyncStateGraphTraceEventID : std::uint16_t {
    TraceStarted, TraceStopped, GraphMutex, GraphPopulation,
    AcceptanceApplied, GraphControlStarted, StateChanged, VersionReclaimed,
    VersionsReclaimed, QueueNodePhase, DependencyBlocked, BuildSubmitted,
    BuildDependencyResolved, BuildStarted, BuildCompleted, BuildRejected,
    AcceptanceQueued, CompletionApplied, CompletionStale, SuspensionRegistered,
    DrainStarted, DrainGpuCollectPhase, GpuNotificationApplied, ExactWaitSatisfied,
    DrainCompleted, RequestReceived, RequestPhase, DependencyDeclared,
    StaticTransactionContents, StaticGroupTransactionLinked, StaticSceneContents,
    RequestConflict, RequestAlreadyDesired, SuccessorQueued, RequestAccepted,
    Invalidated, Cancelled, Released, Published, SuspensionSatisfied,
    ObservationRegistered, ObservationCancelled, KindObservationRegistered,
    ExactWaitRegistered, ExactWaitCancelled, DiagnosePhase,
    ManifestCommitAccepted, ManifestCommitUnchanged, ManifestFragmentCommitted,
    GrassCellIntentAccepted, GrassCompactionShardRequested, GrassDeltaShardRequested,
    GrassCellCompactionBatched, GrassCellDeltaBatched, GrassShardGpuReady,
    GrassCellSelected, GrassSceneBatchRequested, GrassScenePublished,
    StaticGroupDiscovered, StaticGroupBatchQueued, StaticGroupPrepared,
    StaticGroupValidated, StaticGroupWorkerSubmitted, StaticGroupMaterialized,
    StaticGroupBridgeApplied,
    SchedulerTaskQueued, SchedulerTaskAdmitted, SchedulerTaskStarted,
    SchedulerTaskCompleted, SchedulerTaskCancelled, SchedulerTaskRejected,
    SchedulerTaskResubmitted,
    Count
};

using org::async::AsyncStateGraphTracePayload;
using AsyncStateGraphTraceConfig = org::async::AsyncStateGraphTraceConfig<kArtifactKindCount>;

struct AsyncStateGraphTraceReport {
    std::filesystem::path eventsCsv;
    std::filesystem::path staticGroupCsv;
    std::filesystem::path chromeTraceJson;
    std::filesystem::path summaryMarkdown;
    std::uint64_t capturedEvents = 0;
    std::uint64_t droppedEvents = 0;
    std::chrono::microseconds elapsed{};
};

} // namespace br::render
