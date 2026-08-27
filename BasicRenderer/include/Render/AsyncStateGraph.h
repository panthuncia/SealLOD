#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <type_traits>
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
    StaticScene,
    TerrainState,
    BufferVersion,
    FrameManifest,
};

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
    // Owning lifetime pin for this immutable version. Identity comparisons do
    // not include the pin itself.
    std::shared_ptr<const void> lease;
    bool operator==(const ArtifactVersionID& other) const noexcept {
        return address == other.address && revision == other.revision && generation == other.generation;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return revision != 0 && generation != 0;
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
    std::shared_ptr<const void> versionLease;
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
        DependencyInvalidationPolicy::ExactSnapshot, version.generation, version.lease };
}

[[nodiscard]] inline ArtifactRequirement Latest(ArtifactAddress address,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady,
    DependencyPolicy policy = DependencyPolicy::AllOf,
    std::uint32_t alternativeGroup = 0) {
    return { address, 0, readiness, policy, alternativeGroup,
        DependencyInvalidationPolicy::Latest };
}

[[nodiscard]] inline ArtifactRequirement ReadyGate(ArtifactVersionID version,
    ArtifactReadiness readiness = ArtifactReadiness::CpuReady) {
    return { version.address, version.revision, readiness, DependencyPolicy::AllOf, 0,
        DependencyInvalidationPolicy::ReadyGate, version.generation, version.lease };
}

[[nodiscard]] inline ArtifactRequirement LifetimeHold(ArtifactVersionID version) {
    return { version.address, version.revision, ArtifactReadiness::CpuReady,
        DependencyPolicy::Optional, 0, DependencyInvalidationPolicy::LifetimeHold,
        version.generation, version.lease };
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
    std::shared_ptr<const void> versionLease;

    [[nodiscard]] ArtifactVersionID Version() const noexcept {
        return { key, revision, generation, versionLease };
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
    std::function<std::string()> describe;
    std::function<void(std::function<void()>)> subscribe;
    std::function<bool()> cancel;

    [[nodiscard]] bool Submitted() const { return !isSubmitted || isSubmitted(); }
    [[nodiscard]] bool Complete() const { return !isComplete || isComplete(); }
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
    std::shared_ptr<const void> versionLease;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(payload);
    }

    [[nodiscard]] ArtifactVersionID Version() const noexcept {
        return { key, revision, generation, versionLease };
    }
};

template <class T>
[[nodiscard]] ArtifactHandle<T> MakeArtifactHandle(const ArtifactSnapshot& snapshot) {
    return { snapshot.key, snapshot.revision, snapshot.generation, snapshot.readiness,
        snapshot.payload.Get<T>(), snapshot.gpuSubmissions, snapshot.versionLease };
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
    constexpr operator bool() const noexcept {
        return status == ArtifactRequestStatus::Accepted ||
            status == ArtifactRequestStatus::AlreadyDesired;
    }
};

struct ArtifactObservation {
    std::uint64_t subscription = 0;
    std::uint64_t sequence = 0;
    ArtifactSnapshot snapshot;
};

struct ArtifactBuildResult {
    enum class Outcome : std::uint8_t { Ready, NeedsDependencies, RetryAfter, Failed, Cancelled };
    Outcome outcome = Outcome::Failed;
    ArtifactPayload payload;
    ArtifactPayload checkpoint;
    std::vector<ArtifactRequirement> requirements;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    std::chrono::steady_clock::duration retryDelay{};
    std::string error;

    static ArtifactBuildResult Ready(ArtifactPayload payload,
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions = {});
    static ArtifactBuildResult Needs(std::vector<ArtifactRequirement> requirements,
        ArtifactPayload checkpoint = {});
    static ArtifactBuildResult Retry(std::chrono::steady_clock::duration delay,
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
    std::uint64_t archivedVersions = 0;
    std::uint64_t reclaimedVersions = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ArtifactReadiness::Failed) + 1u> stateCounts{};
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
                !result.payload.Get<Output>()) {
                return ArtifactBuildResult::Failure("artifact output type mismatch");
            }
            return result;
        };
        RegisterProducer(kind, std::move(registration));
    }
    ArtifactRequestResult Request(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    ArtifactRequestResult RequestExpressions(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<DependencyExpression> dependencies, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    bool Invalidate(ArtifactKey key, std::uint64_t desiredRevision);
    void Cancel(ArtifactKey key);
    void Release(ArtifactKey key);
    void MarkPublished(ArtifactKey key, std::uint64_t revision);
    void MarkPublished(ArtifactVersionID version);
    void PumpGpuCompletions();
    void SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback);
    [[nodiscard]] std::uint64_t AddReadyCallback(
        std::function<void(const ArtifactSnapshot&)> callback);
    void RemoveReadyCallback(std::uint64_t subscription);
    [[nodiscard]] ArtifactObservation ObserveWithSnapshot(ArtifactKey address,
        std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback);

    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactKey key) const;
    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactVersionID version) const;
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    [[nodiscard]] AsyncStateGraphStats Stats() const;
    [[nodiscard]] std::uint64_t Outstanding(ArtifactKind kind) const;
    void WaitIdle() const;
    void Shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace br::render
