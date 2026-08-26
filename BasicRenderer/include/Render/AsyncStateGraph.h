#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
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

struct ArtifactKey {
    ArtifactKind kind = ArtifactKind::Generic;
    std::uint64_t primaryID = 0;
    std::uint64_t variantID = 0;
    auto operator<=>(const ArtifactKey&) const = default;

    struct Hasher {
        std::size_t operator()(const ArtifactKey& key) const noexcept;
    };
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

enum class DependencyPolicy : std::uint8_t { AllOf, AnyOf, Optional, FallbackAllowed };

struct ArtifactRequirement {
    ArtifactKey key;
    std::uint64_t minimumRevision = 0;
    ArtifactReadiness requiredReadiness = ArtifactReadiness::CpuReady;
    DependencyPolicy policy = DependencyPolicy::AllOf;
    // Non-zero AnyOf requirements sharing a group form one alternative set.
    std::uint32_t alternativeGroup = 0;
};

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
};

struct GpuDependencyToken {
    // Kept opaque in v1 so producers can retain timeline ownership without the
    // state graph depending on a particular RHI backend. Future ORG publication
    // may propagate the same token as an external queue wait.
    std::shared_ptr<const void> timelineOwner;
    std::uint64_t value = 0;
    std::function<bool()> isComplete;
    std::function<std::shared_ptr<const void>()> currentTimelineOwner;
    std::function<std::uint64_t()> currentValue;
    std::function<std::string()> describe;
    std::function<void(std::function<void()>)> subscribe;
    std::function<bool()> cancel;

    [[nodiscard]] bool Complete() const { return !isComplete || isComplete(); }
    [[nodiscard]] std::shared_ptr<const void> TimelineOwner() const {
        return currentTimelineOwner ? currentTimelineOwner() : timelineOwner;
    }
    [[nodiscard]] std::uint64_t TimelineValue() const {
        return currentValue ? currentValue() : value;
    }
	[[nodiscard]] std::string Describe() const { return describe ? describe() : std::string{}; }
    [[nodiscard]] bool Cancel() const { return cancel && cancel(); }
};

std::shared_ptr<const GpuDependencyToken> MakeGpuDependencyToken(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket);

struct ArtifactBuildContext {
    ArtifactKey key;
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    std::vector<ArtifactSnapshot> dependencies;
    ArtifactPayload input;
    ArtifactPayload checkpoint;
    std::function<bool()> stopRequested;
};

enum class ArtifactRequestStatus : std::uint8_t {
    Accepted,
    AlreadyDesired,
    StaleRevision,
    ConflictingRevision,
    ShuttingDown,
};

struct ArtifactRequestResult {
    ArtifactRequestStatus status = ArtifactRequestStatus::ShuttingDown;
    std::uint64_t generation = 0;
    constexpr operator bool() const noexcept {
        return status == ArtifactRequestStatus::Accepted ||
            status == ArtifactRequestStatus::AlreadyDesired;
    }
};

struct ArtifactBuildResult {
    enum class Outcome : std::uint8_t { Ready, NeedsDependencies, RetryAfter, Failed, Cancelled };
    Outcome outcome = Outcome::Failed;
    ArtifactPayload payload;
    ArtifactPayload checkpoint;
    std::vector<ArtifactRequirement> requirements;
    std::shared_ptr<const GpuDependencyToken> gpuDependency;
    std::chrono::steady_clock::duration retryDelay{};
    std::string error;

    static ArtifactBuildResult Ready(ArtifactPayload payload,
        std::shared_ptr<const GpuDependencyToken> gpuDependency = {});
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
};

struct ArtifactDiagnostic {
    ArtifactSnapshot artifact;
    std::uint64_t desiredRevision = 0;
	std::uint64_t generation = 0;
	bool buildInFlight = false;
	bool hasGpuDependency = false;
	bool gpuComplete = false;
	std::uint64_t gpuTimelineValue = 0;
	std::string gpuState;
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
    std::array<std::uint64_t, static_cast<std::size_t>(ArtifactReadiness::Failed) + 1u> stateCounts{};
};

class AsyncStateGraph {
public:
    explicit AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name = "RendererStateGraph");
    ~AsyncStateGraph();
    AsyncStateGraph(const AsyncStateGraph&) = delete;
    AsyncStateGraph& operator=(const AsyncStateGraph&) = delete;

    void RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration);
    ArtifactRequestResult Request(ArtifactKey key, std::uint64_t desiredRevision,
        std::vector<ArtifactRequirement> requirements = {}, ArtifactPayload input = {},
        std::uint64_t requestFingerprint = 0);
    bool Invalidate(ArtifactKey key, std::uint64_t desiredRevision);
    void Cancel(ArtifactKey key);
    void MarkPublished(ArtifactKey key, std::uint64_t revision);
    void PumpGpuCompletions();
    void SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback);

    [[nodiscard]] ArtifactSnapshot Snapshot(ArtifactKey key) const;
    [[nodiscard]] ArtifactDiagnostic Diagnose(ArtifactKey key) const;
    [[nodiscard]] AsyncStateGraphStats Stats() const;
    void WaitIdle() const;
    void Shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> m_impl;
};

} // namespace br::render
