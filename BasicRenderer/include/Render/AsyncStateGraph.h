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

#include "Managers/Singletons/TaskSchedulerManager.h"

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
    TextureImageTable,
    GrassCell,
    GrassShard,
    GrassScratch,
    GrassScene,
};
inline constexpr std::size_t kArtifactKindCount =
    static_cast<std::size_t>(ArtifactKind::GrassScene) + 1u;

struct ArtifactAddress {
    ArtifactKind kind = ArtifactKind::Generic;
    std::uint64_t primaryID = 0;
    std::uint64_t variantID = 0;
    auto operator<=>(const ArtifactAddress&) const = default;

    struct Hasher {
        std::size_t operator()(const ArtifactAddress& key) const noexcept;
    };
};

// ArtifactKey remains as a source-compatible spelling while callers migrate.
// It identifies a logical address only; revisions are never encoded into it.
using ArtifactKey = ArtifactAddress;

struct ArtifactVersionID {
    ArtifactAddress address;
    std::uint64_t revision = 0;
    // Assigned once by the graph. It is never reused and detects stale/ABA handles.
    std::uint64_t generation = 0;
    auto operator<=>(const ArtifactVersionID&) const = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return revision != 0 && generation != 0;
    }
};

// Explicit ownership of an immutable artifact version. Identity values and
// dependency descriptions are deliberately non-owning so copying them into
// diagnostics, signatures, or tombstones cannot retain GPU resources.
class ArtifactLease {
public:
    ArtifactLease() = default;
    explicit ArtifactLease(std::shared_ptr<const void> token) : m_token(std::move(token)) {}

    void reset() noexcept { m_token.reset(); }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_token); }
    [[nodiscard]] long use_count() const noexcept { return m_token.use_count(); }
    [[nodiscard]] const std::shared_ptr<const void>& Token() const noexcept { return m_token; }

private:
    std::shared_ptr<const void> m_token;
};

// Strong, untyped reference to one immutable version. Use this for queued
// orchestration work that has not yet installed a graph dependency. Pure
// ArtifactVersionID values are appropriate only for diagnostics and metadata
// whose enclosing graph recipe or publication bundle already owns the pin.
struct ArtifactVersionHandle {
    ArtifactVersionID version;
    ArtifactLease lease;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(version) && static_cast<bool>(lease);
    }
};

enum class ArtifactReadiness : std::uint8_t {
    Missing,
    Blocked,
    Queued,
    Preparing,
    CpuReady,
    UploadSubmitted,
    GpuReady,
    Published,
    Superseded,
    Cancelled,
    Failed,
};

[[nodiscard]] bool ArtifactReachedMilestone(
    ArtifactReadiness actual, ArtifactReadiness required) noexcept;

// How a requirement participates in dependency selection.
enum class DependencyPolicy : std::uint8_t { AllOf, AnyOf, Optional, FallbackAllowed };

// How a dependency changing after it has been selected affects its consumer.
// Latest preserves the original graph behaviour and is therefore the default.
enum class DependencyInvalidationPolicy : std::uint8_t {
    ExactSnapshot,
    Latest,
    ReadyGate,
    LifetimeHold,
};

struct ArtifactRequirement {
    ArtifactKey key;
    std::uint64_t minimumRevision = 0;
    ArtifactReadiness requiredReadiness = ArtifactReadiness::CpuReady;
    DependencyPolicy policy = DependencyPolicy::AllOf;
    // Non-zero AnyOf requirements sharing a group form one alternative set.
    std::uint32_t alternativeGroup = 0;
    DependencyInvalidationPolicy invalidation = DependencyInvalidationPolicy::Latest;
    // Non-zero for handle-based requirements. This prevents an exact revision
    // from accidentally binding to a different internal incarnation (ABA).
    std::uint64_t requiredGeneration = 0;
    bool operator==(const ArtifactRequirement& other) const noexcept {
        return key == other.key && minimumRevision == other.minimumRevision &&
            requiredReadiness == other.requiredReadiness && policy == other.policy &&
            alternativeGroup == other.alternativeGroup && invalidation == other.invalidation &&
            requiredGeneration == other.requiredGeneration;
    }
};

[[nodiscard]] inline ArtifactRequirement Exact(ArtifactVersionID version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { version.address, version.revision, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::ExactSnapshot, version.generation };
}

[[nodiscard]] inline ArtifactRequirement Exact(const ArtifactVersionHandle& handle,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return Exact(handle.version, readiness, policy, alternativeGroup);
}

[[nodiscard]] inline ArtifactRequirement Latest(ArtifactAddress address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { address, 0, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::Latest };
}

[[nodiscard]] inline ArtifactRequirement LatestAtLeast(ArtifactAddress address,
    std::uint64_t minimumRevision,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { address, minimumRevision, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::Latest };
}

[[nodiscard]] inline ArtifactRequirement ReadyGate(ArtifactVersionID version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return { version.address, version.revision, readiness, DependencyPolicy::AllOf, 0,
        DependencyInvalidationPolicy::ReadyGate, version.generation };
}

// Address-level readiness latch. It selects whichever immutable version of the
// address currently satisfies the milestone and deliberately does not retain or
// invalidate on later versions. Use the handle overload when one exact version
// is the gate.
[[nodiscard]] inline ArtifactRequirement ReadyGate(ArtifactAddress address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return { address, 0, readiness, DependencyPolicy::AllOf, 0,
        DependencyInvalidationPolicy::ReadyGate };
}

[[nodiscard]] inline ArtifactRequirement LifetimeHold(ArtifactVersionID version) {
    return { version.address, version.revision, ArtifactReadiness::CpuReady,
        DependencyPolicy::Optional, 0, DependencyInvalidationPolicy::LifetimeHold,
        version.generation };
}

struct HandleRequirement {
    ArtifactKey key;
    std::uint64_t minimumRevision = 0;
    ArtifactReadiness requiredReadiness = ArtifactReadiness::CpuReady;
};
struct Require { HandleRequirement requirement; };
struct Optional { HandleRequirement requirement; };
struct FirstReady { std::vector<HandleRequirement> alternatives; };
struct AnyReady { std::vector<HandleRequirement> alternatives; };
using DependencyExpression = std::variant<Require, Optional, FirstReady, AnyReady>;

class ArtifactPayload {
public:
    ArtifactPayload() = default;

    template <class T>
    static ArtifactPayload Make(std::shared_ptr<const T> value) {
        ArtifactPayload result;
        result.m_value = std::move(value);
        result.m_type = std::type_index(typeid(T));
        return result;
    }

    template <class T>
    [[nodiscard]] std::shared_ptr<const T> Get() const {
        if (m_type != std::type_index(typeid(T))) return {};
        return std::static_pointer_cast<const T>(m_value);
    }

    [[nodiscard]] bool Valid() const noexcept { return static_cast<bool>(m_value); }
    [[nodiscard]] std::type_index Type() const noexcept { return m_type; }

private:
    std::shared_ptr<const void> m_value;
    std::type_index m_type{ typeid(void) };
};

struct ArtifactSnapshot {
    ArtifactKey key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    ArtifactPayload payload;
    std::shared_ptr<const struct GpuSubmissionSet> gpuSubmissions;
    ArtifactLease lease;

    [[nodiscard]] ArtifactVersionID Version() const noexcept {
        return { key, revision, generation };
    }
};

struct GpuQueueSubmission {
    std::shared_ptr<const void> timelineOwner;
    std::uint64_t value = 0;
    std::function<std::shared_ptr<const void>()> currentTimelineOwner;
    std::function<std::uint64_t()> currentValue;

    [[nodiscard]] std::shared_ptr<const void> TimelineOwner() const {
        return currentTimelineOwner ? currentTimelineOwner() : timelineOwner;
    }
    [[nodiscard]] std::uint64_t TimelineValue() const {
        return currentValue ? currentValue() : value;
    }
};

struct GpuSubmissionSet {
    // Queue timeline/value pairs are intentionally backend-opaque. Published
    // manifests can forward these to ORG without waiting on the CPU.
    std::vector<GpuQueueSubmission> submissions;
    std::function<bool()> isSubmitted;
    std::function<bool()> isComplete;
	std::function<bool()> isFailed;
	std::function<std::string()> failure;
    std::function<std::string()> describe;
    std::function<void(std::function<void()>)> subscribe;
    std::function<bool()> cancel;
    // True only when the backend guarantees a notification for every state
    // transition. Callback-less or best-effort adapters remain on the single
    // graph recovery path during broker migration.
    bool completionNotificationsAreAuthoritative = false;

    [[nodiscard]] bool Submitted() const { return !isSubmitted || isSubmitted(); }
    [[nodiscard]] bool Complete() const { return !isComplete || isComplete(); }
	[[nodiscard]] bool Failed() const { return isFailed && isFailed(); }
	[[nodiscard]] std::string Failure() const {
		return failure ? failure() : std::string{ "GPU submission failed" };
	}
    [[nodiscard]] std::uint64_t MaximumTimelineValue() const {
        std::uint64_t result = 0;
        for (const auto& submission : submissions) {
            result = (std::max)(result, submission.TimelineValue());
        }
        return result;
    }
	[[nodiscard]] std::string Describe() const { return describe ? describe() : std::string{}; }
    [[nodiscard]] bool Cancel() const { return cancel && cancel(); }
};

template <class T>
struct ArtifactHandle {
    ArtifactKey key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    std::shared_ptr<const T> payload;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    ArtifactLease lease;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(payload);
    }

    [[nodiscard]] ArtifactVersionID Version() const noexcept {
        return { key, revision, generation };
    }
};

template <class T>
[[nodiscard]] ArtifactHandle<T> MakeArtifactHandle(const ArtifactSnapshot& snapshot) {
    return { snapshot.key, snapshot.revision, snapshot.generation, snapshot.readiness,
        snapshot.payload.Get<T>(), snapshot.gpuSubmissions, snapshot.lease };
}

std::shared_ptr<const GpuSubmissionSet> MakeGpuSubmissionSet(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket);

struct ArtifactBuildContext {
    ArtifactKey key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    std::vector<ArtifactSnapshot> dependencies;
    ArtifactPayload input;
    ArtifactPayload checkpoint;
    std::function<bool()> stopRequested;

    template <class T>
    [[nodiscard]] ArtifactHandle<T> Dependency(ArtifactKey dependencyKey) const {
        const auto found = std::ranges::find_if(dependencies,
            [&](const ArtifactSnapshot& dependency) {
                return dependency.key == dependencyKey;
            });
        return found == dependencies.end() ? ArtifactHandle<T>{}
                                           : MakeArtifactHandle<T>(*found);
    }
};

enum class ArtifactRequestStatus : std::uint8_t {
    Accepted,
    AlreadyDesired,
    StaleRevision,
    ConflictingRevision,
    MissingFingerprint,
    TypeMismatch,
    ShuttingDown,
};

struct ArtifactRequestResult {
    ArtifactRequestStatus status = ArtifactRequestStatus::ShuttingDown;
    std::uint64_t generation = 0;
    ArtifactVersionID version;
    ArtifactLease lease;
    constexpr operator bool() const noexcept {
        return status == ArtifactRequestStatus::Accepted ||
            status == ArtifactRequestStatus::AlreadyDesired;
    }
    [[nodiscard]] ArtifactVersionHandle Handle() const {
        return { version, lease };
    }
};

struct ArtifactRequest {
    ArtifactKey key;
    std::uint64_t desiredRevision = 0;
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    std::uint64_t requestFingerprint = 0;
};

// A mutable desired-state update. Unlike an exact ArtifactRequest, newer
// intents for the same address may replace queued work before it starts.
using ArtifactIntent = ArtifactRequest;

class ArtifactObservation {
public:
    ArtifactObservation() = default;
    ArtifactObservation(std::uint64_t subscriptionValue, std::uint64_t sequenceValue,
        ArtifactSnapshot snapshotValue, std::function<void()> unsubscribeValue)
        : subscription(subscriptionValue), sequence(sequenceValue),
          snapshot(std::move(snapshotValue)), m_unsubscribe(std::move(unsubscribeValue)) {}
    ~ArtifactObservation() { Reset(); }
    ArtifactObservation(const ArtifactObservation&) = delete;
    ArtifactObservation& operator=(const ArtifactObservation&) = delete;
    ArtifactObservation(ArtifactObservation&& other) noexcept
        : subscription(std::exchange(other.subscription, 0)),
          sequence(other.sequence), snapshot(std::move(other.snapshot)),
          m_unsubscribe(std::move(other.m_unsubscribe)) {}
    ArtifactObservation& operator=(ArtifactObservation&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        subscription = std::exchange(other.subscription, 0);
        sequence = other.sequence;
        snapshot = std::move(other.snapshot);
        m_unsubscribe = std::move(other.m_unsubscribe);
        return *this;
    }
    void Reset() noexcept {
        if (!m_unsubscribe) return;
        auto unsubscribe = std::move(m_unsubscribe);
        subscription = 0;
        unsubscribe();
    }

    std::uint64_t subscription = 0;
    std::uint64_t sequence = 0;
    ArtifactSnapshot snapshot;

private:
    std::function<void()> m_unsubscribe;
};

// Move-only registration for one immutable version milestone. Unlike the
// address/kind observations, this is a correctness primitive: registration and
// the initial exact-version sample are performed under the same graph lock.
class ArtifactAwaiter {
public:
    ArtifactAwaiter() = default;
    ArtifactAwaiter(std::uint64_t subscriptionValue, ArtifactSnapshot snapshotValue,
        std::function<void()> cancelValue)
        : subscription(subscriptionValue), snapshot(std::move(snapshotValue)),
          m_cancel(std::move(cancelValue)) {}
    ~ArtifactAwaiter() { Reset(); }
    ArtifactAwaiter(const ArtifactAwaiter&) = delete;
    ArtifactAwaiter& operator=(const ArtifactAwaiter&) = delete;
    ArtifactAwaiter(ArtifactAwaiter&& other) noexcept
        : subscription(std::exchange(other.subscription, 0)),
          snapshot(std::move(other.snapshot)), m_cancel(std::move(other.m_cancel)) {}
    ArtifactAwaiter& operator=(ArtifactAwaiter&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        subscription = std::exchange(other.subscription, 0);
        snapshot = std::move(other.snapshot);
        m_cancel = std::move(other.m_cancel);
        return *this;
    }
    void Reset() noexcept {
        if (!m_cancel) return;
        auto cancel = std::move(m_cancel);
        subscription = 0;
        cancel();
    }

    std::uint64_t subscription = 0;
    ArtifactSnapshot snapshot;

private:
    std::function<void()> m_cancel;
};

enum class ArtifactSuspensionKind : std::uint8_t {
    ExactDependency,
    Capacity,
    ExternalOperation,
    TransientRetry
};

// A level-triggered reason why a producer cannot make progress.  identity is
// supplied by the provider and must identify one immutable operation/grant.
// Notifications may arrive before the producer returns Suspend(); the graph
// latches them and reconciles the exact artifact generation when registered.
struct ArtifactSuspension {
    ArtifactSuspensionKind kind = ArtifactSuspensionKind::ExternalOperation;
    std::uint64_t identity = 0;
    ArtifactVersionID dependency;
    ArtifactReadiness milestone = ArtifactReadiness::GpuReady;
    std::chrono::steady_clock::time_point deadline{};
    std::uint32_t maximumAttempts = 0;
    std::string reason;

    static ArtifactSuspension Exact(ArtifactVersionID dependency,
        ArtifactReadiness milestone);
    static ArtifactSuspension Capacity(std::uint64_t identity, std::string reason = {});
    static ArtifactSuspension External(std::uint64_t identity, std::string reason = {});
    static ArtifactSuspension Transient(std::uint64_t identity,
        std::chrono::steady_clock::time_point deadline, std::uint32_t maximumAttempts,
        std::string reason);
};

struct ArtifactAcceptanceRegistration {
    TaskLane lane = TaskLane::Streaming;
    TaskDomain domain = TaskDomain::RendererState;
    std::function<void(const ArtifactSnapshot&)> action;
};

struct ArtifactBuildResult {
    enum class Outcome : std::uint8_t {
        Ready, NeedsDependencies, RetryAfter, Failed, Cancelled, Suspended
    };
    Outcome outcome = Outcome::Failed;
    ArtifactPayload payload;
    ArtifactPayload checkpoint;
    std::vector<ArtifactRequirement> requirements;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    std::chrono::steady_clock::duration retryDelay{};
    std::optional<ArtifactSuspension> suspension;
    std::string error;
	ArtifactAcceptanceRegistration acceptance;
	// Runs outside the graph mutex only after this exact producer result has
	// passed generation/dependency validation. Deprecated compatibility adapter;
	// new producers should register acceptance with an explicit lane/domain.
	std::function<void(const ArtifactSnapshot&)> onAccepted;

    static ArtifactBuildResult Ready(ArtifactPayload payload,
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions = {});
    static ArtifactBuildResult Needs(std::vector<ArtifactRequirement> requirements,
        ArtifactPayload checkpoint = {});
    static ArtifactBuildResult Retry(std::chrono::steady_clock::duration delay,
        ArtifactPayload checkpoint = {});
    static ArtifactBuildResult Suspend(ArtifactSuspension suspension,
        ArtifactPayload checkpoint = {});
    static ArtifactBuildResult Failure(std::string error);
    static ArtifactBuildResult Cancelled();
};

using ArtifactProducer = std::function<ArtifactBuildResult(const ArtifactBuildContext&)>;

struct ArtifactProducerRegistration {
    TaskLane lane = TaskLane::Streaming;
    TaskDomain domain = TaskDomain::General;
    std::string taskName;
    ArtifactProducer producer;
    std::type_index inputType{ typeid(void) };
    std::type_index outputType{ typeid(void) };
};

struct ArtifactDiagnostic {
    ArtifactSnapshot artifact;
    std::uint64_t desiredRevision = 0;
    std::vector<ArtifactRequirement> blockers;
    std::string error;
    std::string blockerChain;
    std::chrono::microseconds stateAge{};
};

struct AsyncStateGraphStats {
    std::uint64_t requests = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t buildsStarted = 0;
    std::uint64_t buildsCompleted = 0;
    std::uint64_t staleCompletions = 0;
    std::uint64_t failed = 0;
    std::uint64_t cancelled = 0;
    std::uint64_t cycles = 0;
    std::uint64_t gpuWaiting = 0;
    std::uint64_t retries = 0;
    std::uint64_t queueWaitMicros = 0;
    std::uint64_t buildMicros = 0;
    std::uint64_t gpuWaitMicros = 0;
    std::uint64_t controlQueueWaitMicros = 0;
    std::uint64_t maxControlQueueWaitMicros = 0;
    std::uint64_t completionApplyMicros = 0;
    std::uint64_t maxCompletionApplyMicros = 0;
    std::uint64_t gpuApplyMicros = 0;
    std::uint64_t maxGpuApplyMicros = 0;
    std::uint64_t dependencyEvaluations = 0;
    std::uint64_t coalescedIntents = 0;
    std::uint64_t intentBatches = 0;
    std::uint64_t supersededBuilds = 0;
    std::uint64_t reclaimCandidates = 0;
    std::uint64_t archivedVersions = 0;
    std::uint64_t reclaimedVersions = 0;
    std::uint64_t externallyLeasedVersions = 0;
    std::uint64_t desiredVersions = 0;
    std::uint64_t recipePinnedVersions = 0;
    std::uint64_t unclassifiedRetainedVersions = 0;
	std::uint64_t exactWaiters = 0;
    std::array<std::uint64_t, kArtifactKindCount> intentsByKind{};
    std::array<std::uint64_t, kArtifactKindCount> coalescedByKind{};
    std::array<std::uint64_t, kArtifactKindCount> buildsStartedByKind{};
    std::array<std::uint64_t, kArtifactKindCount> buildsCompletedByKind{};
    std::array<std::uint64_t, kArtifactKindCount> queueWaitMicrosByKind{};
    std::array<std::uint64_t, static_cast<std::size_t>(ArtifactReadiness::Failed) + 1u> stateCounts{};
};

enum class AsyncStateGraphTraceDetail : std::uint8_t {
	Summary,
	Lifecycle,
	FullDependencies
};

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
    Count
};

struct AsyncStateGraphTracePayload {
    std::array<std::uint64_t, 8> values{};
};

struct AsyncStateGraphTraceConfig {
    std::size_t maximumEvents = 1'000'000;
	AsyncStateGraphTraceDetail detail = AsyncStateGraphTraceDetail::Lifecycle;
    bool includeDependencyEvents = true;
    bool includeRetentionEvents = true;
	// Empty/unfiltered traces retain the existing all-artifact behavior. Focused
	// captures can select kinds without adding work to the trace-off path.
	bool filterArtifactKinds = false;
	std::array<bool, kArtifactKindCount> includedKinds{};
};

struct AsyncStateGraphTraceReport {
    std::filesystem::path eventsCsv;
    std::filesystem::path staticGroupCsv;
    std::filesystem::path chromeTraceJson;
    std::filesystem::path summaryMarkdown;
    std::uint64_t capturedEvents = 0;
    std::uint64_t droppedEvents = 0;
    std::chrono::microseconds elapsed{};
};

class AsyncStateGraph {
public:
    explicit AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name = "RendererStateGraph");
    ~AsyncStateGraph();
    AsyncStateGraph(const AsyncStateGraph&) = delete;
    AsyncStateGraph& operator=(const AsyncStateGraph&) = delete;

    void RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration);
    template <class Input, class Output>
    void RegisterTypedProducer(ArtifactKind kind, TaskLane lane, TaskDomain domain,
        std::string taskName,
        std::function<ArtifactBuildResult(const ArtifactBuildContext&,
            std::shared_ptr<const Input>)> producer) {
        ArtifactProducerRegistration registration;
        registration.lane = lane;
        registration.domain = domain;
        registration.taskName = std::move(taskName);
        registration.inputType = std::type_index(typeid(Input));
        registration.outputType = std::type_index(typeid(Output));
        registration.producer = [producer = std::move(producer)](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<Input>();
            if (!input) return ArtifactBuildResult::Failure("artifact input type mismatch");
            auto result = producer(context, input);
            if (result.outcome == ArtifactBuildResult::Outcome::Ready &&
				!result.payload.template Get<Output>()) {
                return ArtifactBuildResult::Failure("artifact output type mismatch");
            }
            return result;
        };
        RegisterProducer(kind, std::move(registration));
    }
    ArtifactRequestResult Request(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    // Derived latest-value intent. Intermediate successors that were submitted
    // through this API and have not started may be replaced; exact Request
    // versions are never coalesced.
    ArtifactRequestStatus SubmitLatestIntent(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    std::vector<ArtifactRequestResult> SubmitLatestIntentBatch(
        std::vector<ArtifactIntent> intents);
    std::vector<ArtifactRequestResult> RequestBatch(std::vector<ArtifactRequest> requests);
    ArtifactRequestResult RequestExpressions(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<DependencyExpression> dependencies, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    bool Invalidate(ArtifactKey key, std::uint64_t desiredRevision);
    void Cancel(ArtifactKey key);
    void Release(ArtifactKey key);
    void MarkPublished(ArtifactKey key, std::uint64_t revision);
    void MarkPublished(ArtifactVersionID version);
    void MarkPublished(std::span<const ArtifactVersionID> versions);
    void PumpGpuCompletions();
    void NotifySuspensionSatisfied(std::uint64_t identity);
    [[nodiscard]] std::function<void(std::uint64_t)> MakeSuspensionNotifier() const;
    void SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback);
    [[nodiscard]] std::uint64_t AddReadyCallback(
        std::function<void(const ArtifactSnapshot&)> callback);
    void RemoveReadyCallback(std::uint64_t subscription);
    [[nodiscard]] ArtifactObservation ObserveWithSnapshot(ArtifactKey address,
        std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback);
	// Installs one level-triggered wakeup for a resource family. Consumers still
	// reconcile exact versions from graph state; the notification is never an
	// ownership-transfer event.
	[[nodiscard]] ArtifactObservation ObserveKind(ArtifactKind kind,
		std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback);
	[[nodiscard]] ArtifactAwaiter AwaitExact(ArtifactVersionHandle handle,
		ArtifactReadiness milestone, TaskLane lane, TaskDomain domain,
		std::function<void(const ArtifactSnapshot&)> continuation);

    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactKey key) const;
    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactVersionID version) const;
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    [[nodiscard]] AsyncStateGraphStats Stats() const;
    [[nodiscard]] std::uint64_t Outstanding(ArtifactKind kind) const;
    void StartTrace(AsyncStateGraphTraceConfig config = {});
    [[nodiscard]] bool TraceActive() const;
    AsyncStateGraphTraceReport StopTraceAndWriteReport(const std::filesystem::path& outputDirectory);
    void TraceEvent(AsyncStateGraphTraceEventID event, ArtifactAddress address,
        std::uint64_t revision = 0, std::uint64_t generation = 0,
        AsyncStateGraphTracePayload payload = {}, ArtifactAddress related = {},
        std::uint64_t relatedRevision = 0);
    void WaitIdle() const;
    void Shutdown();

private:
    ArtifactRequestResult RequestInternal(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
        std::uint64_t requestFingerprint, bool coalescibleIntent, bool callerOwnsMutex = false);
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace br::render
