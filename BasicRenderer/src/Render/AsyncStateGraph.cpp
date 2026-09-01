#include "Render/AsyncStateGraph.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#endif

#include <spdlog/spdlog.h>
#include <tbb/concurrent_queue.h>

#include <BasicTelemetry/Telemetry.h>

#include "Render/Runtime/StreamingUploadTypes.h"
#include "Render/StaticStateArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Managers/SerializedTaskPump.h"

namespace br::render {

namespace {
std::atomic_uint64_t g_nextArtifactSuspensionIdentity{ 1 };

std::uint64_t CurrentThreadCpuMicros() noexcept {
#ifdef _WIN32
	FILETIME created{}, exited{}, kernel{}, user{};
	if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
	ULARGE_INTEGER kernelTicks{}, userTicks{};
	kernelTicks.LowPart = kernel.dwLowDateTime;
	kernelTicks.HighPart = kernel.dwHighDateTime;
	userTicks.LowPart = user.dwLowDateTime;
	userTicks.HighPart = user.dwHighDateTime;
	return (kernelTicks.QuadPart + userTicks.QuadPart) / 10u;
#else
	timespec value{};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return 0;
	return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000u +
		static_cast<std::uint64_t>(value.tv_nsec / 1'000u);
#endif
}
}
bool ArtifactReachedMilestone(ArtifactReadiness actual, ArtifactReadiness required) noexcept {
    if (actual == ArtifactReadiness::Published) return true;
    if (actual == ArtifactReadiness::Failed || actual == ArtifactReadiness::Cancelled ||
        actual == ArtifactReadiness::Superseded) return false;
    return static_cast<unsigned>(actual) >= static_cast<unsigned>(required);
}

namespace {

bool Satisfies(ArtifactReadiness actual, ArtifactReadiness required) {
    return ArtifactReachedMilestone(actual, required);
}

std::string KeyString(const ArtifactKey& key) {
    return std::format("{}:{}:{}", static_cast<unsigned>(key.kind), key.primaryID, key.variantID);
}

std::string_view KindName(ArtifactKind kind) {
    static constexpr std::string_view names[]{ "Generic", "TextureBinding", "Material",
        "MaterialTable", "MaterialUsageBatch", "Mesh", "MeshTable", "DrawRecordPage",
        "ActiveDrawList", "ViewLifetime", "IndirectWorkload", "StaticTransaction",
        "StaticScenePage", "StaticScene", "TerrainState", "BufferVersion", "FrameManifest", "StaticGroup",
        "StaticTemplate", "TextureImageTable", "GrassCell", "GrassShard", "GrassScratch", "GrassScene" };
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(names) ? names[index] : "Unknown";
}

std::string_view ReadinessName(ArtifactReadiness readiness) {
    static constexpr std::string_view names[]{ "Missing", "Blocked", "Queued", "Preparing",
        "CpuReady", "UploadSubmitted", "GpuReady", "Published", "Superseded", "Cancelled",
        "Failed" };
    const auto index = static_cast<std::size_t>(readiness);
    return index < std::size(names) ? names[index] : "Unknown";
}

void HashRequestValue(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}

std::uint64_t CanonicalRequestFingerprint(const ArtifactKey& key,
    std::uint64_t revision, const std::vector<ArtifactRequirement>& requirements) {
    std::uint64_t hash = 1469598103934665603ull;
    HashRequestValue(hash, static_cast<std::uint64_t>(key.kind));
    HashRequestValue(hash, key.primaryID);
    HashRequestValue(hash, key.variantID);
    HashRequestValue(hash, revision);
    for (const auto& requirement : requirements) {
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.key.kind));
        HashRequestValue(hash, requirement.key.primaryID);
        HashRequestValue(hash, requirement.key.variantID);
        HashRequestValue(hash, requirement.minimumRevision);
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.requiredReadiness));
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.policy));
        HashRequestValue(hash, requirement.alternativeGroup);
        HashRequestValue(hash, static_cast<std::uint64_t>(requirement.invalidation));
        HashRequestValue(hash, requirement.requiredGeneration);
    }
    return hash == 0 ? 1u : hash;
}

struct GraphTraceEvent {
    std::int64_t timestampNanoseconds = 0;
    ArtifactKey key{};
    ArtifactKey related{};
    AsyncStateGraphTracePayload payload{};
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    std::uint64_t relatedRevision = 0;
    std::int64_t durationMicros = 0;
    AsyncStateGraphTraceEventID event = AsyncStateGraphTraceEventID::TraceStarted;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
};
static_assert(std::is_trivially_copyable_v<GraphTraceEvent>);

struct ExpandedGraphTraceEvent {
    std::uint64_t sequence = 0;
    std::int64_t timestampMicros = 0;
    std::uint64_t thread = 0;
    std::string event;
    std::string subsystem;
    ArtifactKey key{};
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactKey related{};
    std::uint64_t relatedRevision = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    std::int64_t durationMicros = 0;
    std::string detail;
};

constexpr std::uint64_t StableTraceID(std::string_view value) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

std::uint64_t TraceCorrelationID(const ArtifactKey& key, std::uint64_t revision,
    std::uint64_t generation) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    const auto append = [&hash](std::uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    append(static_cast<std::uint64_t>(key.kind));
    append(key.primaryID);
    append(key.variantID);
    append(revision);
    append(generation);
    return hash == 0 ? 1u : hash;
}

std::string_view TraceEventName(AsyncStateGraphTraceEventID event) {
#define SARP_TRACE_EVENT_NAME(name) case AsyncStateGraphTraceEventID::name: return #name
    switch (event) {
    SARP_TRACE_EVENT_NAME(TraceStarted); SARP_TRACE_EVENT_NAME(TraceStopped);
    SARP_TRACE_EVENT_NAME(GraphMutex); SARP_TRACE_EVENT_NAME(GraphPopulation);
    SARP_TRACE_EVENT_NAME(AcceptanceApplied); SARP_TRACE_EVENT_NAME(GraphControlStarted);
    SARP_TRACE_EVENT_NAME(StateChanged); SARP_TRACE_EVENT_NAME(VersionReclaimed);
    SARP_TRACE_EVENT_NAME(VersionsReclaimed); SARP_TRACE_EVENT_NAME(QueueNodePhase);
    SARP_TRACE_EVENT_NAME(DependencyBlocked); SARP_TRACE_EVENT_NAME(BuildSubmitted);
    SARP_TRACE_EVENT_NAME(BuildDependencyResolved); SARP_TRACE_EVENT_NAME(BuildStarted);
    SARP_TRACE_EVENT_NAME(BuildCompleted); SARP_TRACE_EVENT_NAME(BuildRejected);
    SARP_TRACE_EVENT_NAME(AcceptanceQueued); SARP_TRACE_EVENT_NAME(CompletionApplied);
    SARP_TRACE_EVENT_NAME(CompletionStale); SARP_TRACE_EVENT_NAME(SuspensionRegistered);
    SARP_TRACE_EVENT_NAME(DrainStarted); SARP_TRACE_EVENT_NAME(DrainGpuCollectPhase);
    SARP_TRACE_EVENT_NAME(GpuNotificationApplied); SARP_TRACE_EVENT_NAME(ExactWaitSatisfied);
    SARP_TRACE_EVENT_NAME(DrainCompleted); SARP_TRACE_EVENT_NAME(RequestReceived);
    SARP_TRACE_EVENT_NAME(RequestPhase); SARP_TRACE_EVENT_NAME(DependencyDeclared);
    SARP_TRACE_EVENT_NAME(StaticTransactionContents);
    SARP_TRACE_EVENT_NAME(StaticGroupTransactionLinked); SARP_TRACE_EVENT_NAME(StaticSceneContents);
    SARP_TRACE_EVENT_NAME(RequestConflict); SARP_TRACE_EVENT_NAME(RequestAlreadyDesired);
    SARP_TRACE_EVENT_NAME(SuccessorQueued); SARP_TRACE_EVENT_NAME(RequestAccepted);
    SARP_TRACE_EVENT_NAME(Invalidated); SARP_TRACE_EVENT_NAME(Cancelled);
    SARP_TRACE_EVENT_NAME(Released); SARP_TRACE_EVENT_NAME(Published);
    SARP_TRACE_EVENT_NAME(SuspensionSatisfied); SARP_TRACE_EVENT_NAME(ObservationRegistered);
    SARP_TRACE_EVENT_NAME(ObservationCancelled); SARP_TRACE_EVENT_NAME(KindObservationRegistered);
    SARP_TRACE_EVENT_NAME(ExactWaitRegistered); SARP_TRACE_EVENT_NAME(ExactWaitCancelled);
    SARP_TRACE_EVENT_NAME(DiagnosePhase); SARP_TRACE_EVENT_NAME(ManifestCommitAccepted);
    SARP_TRACE_EVENT_NAME(ManifestCommitUnchanged); SARP_TRACE_EVENT_NAME(ManifestFragmentCommitted);
    SARP_TRACE_EVENT_NAME(GrassCellIntentAccepted); SARP_TRACE_EVENT_NAME(GrassCompactionShardRequested);
    SARP_TRACE_EVENT_NAME(GrassDeltaShardRequested); SARP_TRACE_EVENT_NAME(GrassCellCompactionBatched);
    SARP_TRACE_EVENT_NAME(GrassCellDeltaBatched); SARP_TRACE_EVENT_NAME(GrassShardGpuReady);
    SARP_TRACE_EVENT_NAME(GrassCellSelected); SARP_TRACE_EVENT_NAME(GrassSceneBatchRequested);
    SARP_TRACE_EVENT_NAME(GrassScenePublished); SARP_TRACE_EVENT_NAME(StaticGroupDiscovered);
    SARP_TRACE_EVENT_NAME(StaticGroupBatchQueued); SARP_TRACE_EVENT_NAME(StaticGroupPrepared);
    SARP_TRACE_EVENT_NAME(StaticGroupValidated); SARP_TRACE_EVENT_NAME(StaticGroupWorkerSubmitted);
    SARP_TRACE_EVENT_NAME(StaticGroupMaterialized); SARP_TRACE_EVENT_NAME(StaticGroupBridgeApplied);
    SARP_TRACE_EVENT_NAME(SchedulerTaskQueued); SARP_TRACE_EVENT_NAME(SchedulerTaskAdmitted);
    SARP_TRACE_EVENT_NAME(SchedulerTaskStarted); SARP_TRACE_EVENT_NAME(SchedulerTaskCompleted);
    SARP_TRACE_EVENT_NAME(SchedulerTaskCancelled); SARP_TRACE_EVENT_NAME(SchedulerTaskRejected);
    SARP_TRACE_EVENT_NAME(SchedulerTaskResubmitted);
    case AsyncStateGraphTraceEventID::Count: break;
    }
#undef SARP_TRACE_EVENT_NAME
    return "Unknown";
}

enum class GraphMutexPhase : std::uint8_t {
    AcceptanceScheduleFailure, AcceptanceDequeue, AcceptanceCompletionEnqueue,
    AcceptanceDispatchEnqueue, DelayedDrainState, ProducerCompletionEnqueue,
    DrainGpuCollect, DrainApply, Reclaim, RegisterProducer, Request, RequestBatch,
    CancelLookup, CancelApply, Invalidate, MarkPublishedRevision,
    MarkPublishedVersions, GpuCompletionScan, SuspensionSatisfied,
    ReadyCallbackSet, ReadyCallbackAdd, ReadyCallbackRemove,
    ObservationRegister, ObservationRemove, ExactWaiterRegister, ExactWaiterRemove,
    Snapshot, Diagnose, Stats, Outstanding, WaitIdle, RecoveryResume, Shutdown, Count
};

constexpr std::string_view GraphMutexPhaseName(GraphMutexPhase phase) {
    switch (phase) {
    case GraphMutexPhase::AcceptanceScheduleFailure: return "AcceptanceScheduleFailure";
    case GraphMutexPhase::AcceptanceDequeue: return "AcceptanceDequeue";
    case GraphMutexPhase::AcceptanceCompletionEnqueue: return "AcceptanceCompletionEnqueue";
    case GraphMutexPhase::AcceptanceDispatchEnqueue: return "AcceptanceDispatchEnqueue";
    case GraphMutexPhase::DelayedDrainState: return "DelayedDrainState";
    case GraphMutexPhase::ProducerCompletionEnqueue: return "ProducerCompletionEnqueue";
    case GraphMutexPhase::DrainGpuCollect: return "DrainGpuCollect";
    case GraphMutexPhase::DrainApply: return "DrainApply";
	case GraphMutexPhase::Reclaim: return "Reclaim";
    case GraphMutexPhase::RegisterProducer: return "RegisterProducer";
    case GraphMutexPhase::Request: return "Request";
    case GraphMutexPhase::RequestBatch: return "RequestBatch";
    case GraphMutexPhase::CancelLookup: return "CancelLookup";
    case GraphMutexPhase::CancelApply: return "CancelApply";
    case GraphMutexPhase::Invalidate: return "Invalidate";
    case GraphMutexPhase::MarkPublishedRevision: return "MarkPublishedRevision";
    case GraphMutexPhase::MarkPublishedVersions: return "MarkPublishedVersions";
    case GraphMutexPhase::GpuCompletionScan: return "GpuCompletionScan";
    case GraphMutexPhase::SuspensionSatisfied: return "SuspensionSatisfied";
    case GraphMutexPhase::ReadyCallbackSet: return "ReadyCallbackSet";
    case GraphMutexPhase::ReadyCallbackAdd: return "ReadyCallbackAdd";
    case GraphMutexPhase::ReadyCallbackRemove: return "ReadyCallbackRemove";
    case GraphMutexPhase::ObservationRegister: return "ObservationRegister";
    case GraphMutexPhase::ObservationRemove: return "ObservationRemove";
    case GraphMutexPhase::ExactWaiterRegister: return "ExactWaiterRegister";
    case GraphMutexPhase::ExactWaiterRemove: return "ExactWaiterRemove";
    case GraphMutexPhase::Snapshot: return "Snapshot";
    case GraphMutexPhase::Diagnose: return "Diagnose";
    case GraphMutexPhase::Stats: return "Stats";
    case GraphMutexPhase::Outstanding: return "Outstanding";
    case GraphMutexPhase::WaitIdle: return "WaitIdle";
    case GraphMutexPhase::RecoveryResume: return "RecoveryResume";
    case GraphMutexPhase::Shutdown: return "Shutdown";
    case GraphMutexPhase::Count: break;
    }
    return "Unknown";
}

struct GraphMutexCounts {
    std::uint64_t nodes = 0;
    std::uint64_t versions = 0;
    std::uint64_t pending = 0;
    std::uint64_t completions = 0;
    std::uint64_t gpuRecovery = 0;
    std::uint64_t waiters = 0;
};

struct GraphMutexAggregate {
    std::uint64_t count = 0;
    std::uint64_t totalWaitMicros = 0;
    std::uint64_t maximumWaitMicros = 0;
    std::uint64_t totalHoldMicros = 0;
    std::uint64_t maximumHoldMicros = 0;
	std::uint64_t totalHoldCpuMicros = 0;
	std::uint64_t maximumHoldCpuMicros = 0;
    GraphMutexCounts maximumWaitCounts;
    GraphMutexCounts maximumHoldCounts;
};

struct ThreadGraphMutexAggregate {
    GraphMutexAggregate value;

    void Record(std::uint64_t waitMicros, std::uint64_t holdMicros,
		std::uint64_t holdCpuMicros,
        const GraphMutexCounts& counts) {
        ++value.count;
        value.totalWaitMicros += waitMicros;
        value.totalHoldMicros += holdMicros;
		value.totalHoldCpuMicros += holdCpuMicros;
		value.maximumHoldCpuMicros = (std::max)(value.maximumHoldCpuMicros, holdCpuMicros);
        if (waitMicros > value.maximumWaitMicros) {
            value.maximumWaitMicros = waitMicros;
            value.maximumWaitCounts = counts;
        }
        if (holdMicros > value.maximumHoldMicros) {
            value.maximumHoldMicros = holdMicros;
            value.maximumHoldCounts = counts;
        }
    }
};

std::string CsvField(std::string_view value) {
    std::string result = "\"";
    for (const char character : value) {
        if (character == '"') result += '"';
        result += character;
    }
    result += '"';
    return result;
}

std::string JsonString(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

std::string_view TraceLabelName(std::uint64_t id) {
#define SARP_TRACE_LABEL(text) if (id == StableTraceID(text)) return text
    SARP_TRACE_LABEL("latch_ready_gates"); SARP_TRACE_LABEL("dependencies_satisfied");
    SARP_TRACE_LABEL("notification_queue"); SARP_TRACE_LABEL("recovery_queue");
    SARP_TRACE_LABEL("validate_and_lookup"); SARP_TRACE_LABEL("store_and_trace_request");
    SARP_TRACE_LABEL("remove_waiter_edges"); SARP_TRACE_LABEL("defer_old_version");
    SARP_TRACE_LABEL("reset_version_state"); SARP_TRACE_LABEL("install_waiter_edges");
    SARP_TRACE_LABEL("detect_cycle"); SARP_TRACE_LABEL("queue_and_supersede");
    SARP_TRACE_LABEL("install_new_recipe"); SARP_TRACE_LABEL("clear_node_state");
    SARP_TRACE_LABEL("reset_control_state"); SARP_TRACE_LABEL("reset_gpu_state");
    SARP_TRACE_LABEL("completion_validate"); SARP_TRACE_LABEL("completion_payload");
    SARP_TRACE_LABEL("completion_state"); SARP_TRACE_LABEL("completion_store");
    SARP_TRACE_LABEL("completion_wake"); SARP_TRACE_LABEL("completion_promote");
    SARP_TRACE_LABEL("apply_gpu"); SARP_TRACE_LABEL("apply_retries");
    SARP_TRACE_LABEL("apply_completions"); SARP_TRACE_LABEL("apply_waiter_wakes");
    SARP_TRACE_LABEL("apply_pending_builds"); SARP_TRACE_LABEL("apply_exact_waiters");
	SARP_TRACE_LABEL("dependency_snapshots"); SARP_TRACE_LABEL("wake_waiter");
	SARP_TRACE_LABEL("prepare_acceptance"); SARP_TRACE_LABEL("apply_completion_total");
	SARP_TRACE_LABEL("completion_stale_validate");
    SARP_TRACE_LABEL("apply_tail");
    SARP_TRACE_LABEL("collect_direct_blockers");
    SARP_TRACE_LABEL("append_blocker_chain"); SARP_TRACE_LABEL("ready-at-registration");
#undef SARP_TRACE_LABEL
    return {};
}

std::string TraceDetail(const GraphTraceEvent& event) {
    const auto& v = event.payload.values;
    const auto label = [&] {
        const auto known = TraceLabelName(v[0]);
        return known.empty() ? std::format("id={}", v[0]) : std::string(known);
    };
    using E = AsyncStateGraphTraceEventID;
    switch (event.event) {
    case E::GraphMutex:
		return std::format("phase={} wait_us={} hold_us={} hold_cpu_us={}",
			v[0], v[1], v[2], v[3]);
    case E::GraphPopulation:
        return std::format("nodes={} versions={} pending={} completions={} gpu_recovery={} waiters={}",
            v[0], v[1], v[2], v[3], v[4], v[5]);
    case E::StateChanged: return std::format("{}->{}", v[0], v[1]);
    case E::QueueNodePhase: case E::DrainGpuCollectPhase: case E::RequestPhase:
    case E::DiagnosePhase: return label();
    case E::DependencyBlocked:
        return std::format("policy={} milestone={} generation={}", v[0], v[1], v[2]);
    case E::BuildSubmitted: case E::BuildStarted:
        return std::format("task_kind={} correlation={}", v[0], v[1]);
    case E::BuildDependencyResolved:
        return std::format("dependency_generation={}", v[0]);
    case E::BuildCompleted:
        return std::format("outcome={} task_kind={} correlation={}", v[0], v[1], v[2]);
    case E::CompletionStale:
        return std::format("current_generation={}", v[0]);
    case E::SuspensionRegistered:
        return std::format("kind={} identity={} milestone={} reason_id={}", v[0], v[1], v[2], v[3]);
    case E::DrainCompleted:
        return std::format("transitions={} builds={} ready={} gpuSignals={}", v[0], v[1], v[2], v[3]);
    case E::RequestReceived:
        return std::format("dependencies={} fingerprint={}", v[0], v[1]);
    case E::DependencyDeclared:
        return std::format("policy={} invalidation={} alternative_group={} generation={}", v[0], v[1], v[2], v[3]);
    case E::StaticTransactionContents:
        return std::format("groups={} placements={} draws={} active={} stream_generation={}", v[0], v[1], v[2], v[3], v[4]);
    case E::StaticGroupTransactionLinked:
        return std::format("placements={} draws={} active={}", v[0], v[1], v[2]);
    case E::StaticSceneContents:
        return std::format("groups={} desired_placements={} materialized_placements={} retired_placements={}", v[0], v[1], v[2], v[3]);
    case E::ObservationRegistered: case E::ObservationCancelled: case E::ExactWaitCancelled:
        return std::format("subscription={}", v[0]);
    case E::ExactWaitRegistered:
        return std::format("subscription={} milestone={}", v[0], v[1]);
    case E::ExactWaitSatisfied:
        return v[1] ? "ready-at-registration" : std::format("subscription={}", v[0]);
    case E::ManifestCommitAccepted: case E::ManifestCommitUnchanged:
        return std::format("frame_slot={}", v[0]);
    case E::ManifestFragmentCommitted:
        return std::format("epoch={} frame_slot={} fragment={}", v[0], v[1], v[2]);
    case E::GrassCellIntentAccepted: return std::format("bytes={} tier={}", v[0], v[1]);
    case E::GrassCompactionShardRequested: case E::GrassDeltaShardRequested:
        return std::format("cells={} estimatedBytes={}", v[0], v[1]);
    case E::GrassCellCompactionBatched: case E::GrassCellDeltaBatched:
        return std::format("shardPrimary={} shardVariant={}", v[0], v[1]);
    case E::GrassShardGpuReady:
        return std::format("selectedCells={} compaction={}", v[0], v[1] != 0);
    case E::GrassCellSelected:
        if (v[2]) return "compaction=true";
        return std::format("shardPrimary={} shardVariant={}", v[0], v[1]);
    case E::GrassSceneBatchRequested:
        return std::format("shards={} cells={} scratchCells={}", v[0], v[1], v[2]);
    case E::GrassScenePublished: return std::format("published={}", v[0] != 0);
    case E::StaticGroupDiscovered:
        return std::format("placements={} world={} cell_x={} cell_y={} residency_class={}",
            v[0], v[1], static_cast<std::int64_t>(v[2]), static_cast<std::int64_t>(v[3]), v[4]);
    case E::StaticGroupBatchQueued:
        return std::format("ticket={} batch_groups={}", v[0], v[1]);
    case E::StaticGroupPrepared:
        return std::format("placements={} build_us={} prepare_us={}", v[0], v[1], v[2]);
    case E::StaticGroupValidated:
        return std::format("ticket={} decision={}", v[0], v[1] ? "publish" : "cancel");
    case E::StaticGroupWorkerSubmitted: return std::format("placements={}", v[0]);
    case E::StaticGroupMaterialized:
        return std::format("ticket={} groups={} bytes={}", v[0], v[1], v[2]);
    case E::StaticGroupBridgeApplied:
        return std::format("placements={} ticket={} lod_block={}", v[0], v[1], v[2]);
    case E::SchedulerTaskQueued: case E::SchedulerTaskAdmitted:
    case E::SchedulerTaskStarted: case E::SchedulerTaskCompleted:
    case E::SchedulerTaskCancelled: case E::SchedulerTaskRejected:
    case E::SchedulerTaskResubmitted:
        return std::format(
            "task_id={} task_kind={} correlation={} domain={} lane={} work_class={} reason={} "
            "admission_group={} admission_key={} queue_depth={} active={} queue_wait_us={} "
            "execution_us={} outcome={}",
            event.revision, v[0], event.generation, v[1], v[2], v[3], v[4],
            v[5], v[6], v[7], event.related.primaryID, event.related.variantID,
            event.durationMicros, event.relatedRevision);
    default: break;
    }
    if (v[0] != 0) return std::format("value0={}", v[0]);
    return {};
}

class GraphTraceSession {
    static constexpr std::size_t kEventsPerChunk = 1'024;
    struct TraceChunk {
        std::array<GraphTraceEvent, kEventsPerChunk> events{};
        std::size_t size = 0;
        std::size_t capacity = 0;
    };
    struct TraceShard {
        std::vector<std::unique_ptr<TraceChunk>> chunks;
        TraceChunk* current = nullptr;
        std::uint64_t thread = 0;
        std::uint64_t dropped = 0;
        std::array<ThreadGraphMutexAggregate,
            static_cast<std::size_t>(GraphMutexPhase::Count)> mutexAggregates{};
    };

public:
    explicit GraphTraceSession(AsyncStateGraphTraceConfig config)
        : m_config(config), m_started(std::chrono::steady_clock::now()) {}

    void Record(AsyncStateGraphTraceEventID event, ArtifactKey key = {}, std::uint64_t revision = 0,
        std::uint64_t generation = 0, ArtifactReadiness readiness = ArtifactReadiness::Missing,
        std::int64_t durationMicros = 0, AsyncStateGraphTracePayload payload = {}, ArtifactKey related = {},
        std::uint64_t relatedRevision = 0) {
		const bool schedulerEvent = event >= AsyncStateGraphTraceEventID::SchedulerTaskQueued &&
			event <= AsyncStateGraphTraceEventID::SchedulerTaskResubmitted;
		if (m_config.filterArtifactKinds && !schedulerEvent) {
			const auto included = [this](ArtifactKind kind) {
				const auto index = static_cast<std::size_t>(kind);
				return index < m_config.includedKinds.size() && m_config.includedKinds[index];
			};
			if (!included(key.kind) && !included(related.kind)) return;
		}
        auto* shard = ThreadShard();
        if (!shard->current || shard->current->size == shard->current->capacity) {
            const auto begin = m_reservedEvents.fetch_add(kEventsPerChunk, std::memory_order_relaxed);
            if (begin >= m_config.maximumEvents) { ++shard->dropped; return; }
            auto chunk = std::make_unique<TraceChunk>();
            chunk->capacity = (std::min)(kEventsPerChunk, m_config.maximumEvents - begin);
            shard->current = chunk.get();
            shard->chunks.push_back(std::move(chunk));
        }
        GraphTraceEvent record;
        record.timestampNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - m_started).count();
        record.event = event;
        record.key = key;
        record.revision = revision;
        record.generation = generation;
        record.related = related;
        record.relatedRevision = relatedRevision;
        record.readiness = readiness;
        record.durationMicros = durationMicros;
        record.payload = payload;
        shard->current->events[shard->current->size++] = record;
    }

    void RecordSchedulerEvent(const TaskTraceEvent& event) {
        AsyncStateGraphTraceEventID graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskQueued;
        switch (event.event) {
        case TaskTraceEventID::Queued: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskQueued; break;
        case TaskTraceEventID::Admitted: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskAdmitted; break;
        case TaskTraceEventID::Started: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskStarted; break;
        case TaskTraceEventID::Completed: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskCompleted; break;
        case TaskTraceEventID::Cancelled: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskCancelled; break;
        case TaskTraceEventID::Rejected: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskRejected; break;
        case TaskTraceEventID::Resubmitted: graphEvent = AsyncStateGraphTraceEventID::SchedulerTaskResubmitted; break;
        }
        Record(graphEvent, {}, event.taskID, event.metadata.correlationID,
            ArtifactReadiness::Missing, static_cast<std::int64_t>(event.executionMicros),
            { { event.metadata.taskKind, static_cast<std::uint64_t>(event.domain),
                static_cast<std::uint64_t>(event.lane), event.metadata.workClass,
                event.metadata.schedulingReason, event.metadata.admissionGroup,
                event.metadata.admissionKey, event.queuedDepth } },
            { ArtifactKind::Generic, event.activeCount, event.queueWaitMicros }, event.outcome);
    }

    void RecordMutex(GraphMutexPhase phase, std::uint64_t waitMicros,
        std::uint64_t holdMicros, std::uint64_t holdCpuMicros,
		const GraphMutexCounts& counts) {
        ThreadShard()->mutexAggregates[static_cast<std::size_t>(phase)].Record(
			waitMicros, holdMicros, holdCpuMicros, counts);
        constexpr std::uint64_t slowSampleMicros = 2'000;
        if (waitMicros < slowSampleMicros && holdMicros < slowSampleMicros) return;
        Record(AsyncStateGraphTraceEventID::GraphMutex, {}, 0, 0, ArtifactReadiness::Missing,
            static_cast<std::int64_t>(waitMicros + holdMicros),
			{ { static_cast<std::uint64_t>(phase), waitMicros, holdMicros, holdCpuMicros } });
    }

    AsyncStateGraphTraceReport Write(const std::filesystem::path& directory) {
        struct OrderedRecord {
            GraphTraceEvent event;
            std::uint64_t thread = 0;
            std::uint64_t localSequence = 0;
        };
        std::vector<OrderedRecord> records;
        std::uint64_t dropped = 0;
        std::uint64_t chunkCount = 0;
        std::array<GraphMutexAggregate,
            static_cast<std::size_t>(GraphMutexPhase::Count)> mutexAggregates{};
        std::vector<std::shared_ptr<TraceShard>> shards;
        std::shared_ptr<TraceShard> shard;
        while (m_shards.try_pop(shard)) shards.push_back(std::move(shard));
        for (const auto& traceShard : shards) {
            std::uint64_t localSequence = 0;
            dropped += traceShard->dropped;
            chunkCount += traceShard->chunks.size();
            for (const auto& chunk : traceShard->chunks) {
                for (std::size_t index = 0; index < chunk->size; ++index)
                    records.push_back({ chunk->events[index], traceShard->thread, localSequence++ });
            }
        }
        for (const auto& traceShard : shards) {
            for (std::size_t index = 0; index < mutexAggregates.size(); ++index) {
                const auto& source = traceShard->mutexAggregates[index].value;
                auto& destination = mutexAggregates[index];
                destination.count += source.count;
                destination.totalWaitMicros += source.totalWaitMicros;
                destination.totalHoldMicros += source.totalHoldMicros;
				destination.totalHoldCpuMicros += source.totalHoldCpuMicros;
				destination.maximumHoldCpuMicros = (std::max)(
					destination.maximumHoldCpuMicros, source.maximumHoldCpuMicros);
                if (source.maximumWaitMicros > destination.maximumWaitMicros) {
                    destination.maximumWaitMicros = source.maximumWaitMicros;
                    destination.maximumWaitCounts = source.maximumWaitCounts;
                }
                if (source.maximumHoldMicros > destination.maximumHoldMicros) {
                    destination.maximumHoldMicros = source.maximumHoldMicros;
                    destination.maximumHoldCounts = source.maximumHoldCounts;
                }
            }
        }
        std::ranges::sort(records, [](const OrderedRecord& left, const OrderedRecord& right) {
            if (left.event.timestampNanoseconds != right.event.timestampNanoseconds)
                return left.event.timestampNanoseconds < right.event.timestampNanoseconds;
            if (left.thread != right.thread) return left.thread < right.thread;
            return left.localSequence < right.localSequence;
        });
        GraphMutexCounts latestPopulation{};
        GraphMutexCounts maximumPopulation{};
        std::uint64_t populationSnapshotCount = 0;
        for (const auto& source : records) {
            if (source.event.event != AsyncStateGraphTraceEventID::GraphPopulation) continue;
            const auto& values = source.event.payload.values;
            latestPopulation = { values[0], values[1], values[2], values[3], values[4], values[5] };
            maximumPopulation.nodes = (std::max)(maximumPopulation.nodes, latestPopulation.nodes);
            maximumPopulation.versions = (std::max)(maximumPopulation.versions, latestPopulation.versions);
            maximumPopulation.pending = (std::max)(maximumPopulation.pending, latestPopulation.pending);
            maximumPopulation.completions = (std::max)(maximumPopulation.completions, latestPopulation.completions);
            maximumPopulation.gpuRecovery = (std::max)(maximumPopulation.gpuRecovery, latestPopulation.gpuRecovery);
            maximumPopulation.waiters = (std::max)(maximumPopulation.waiters, latestPopulation.waiters);
            ++populationSnapshotCount;
        }
        std::vector<ExpandedGraphTraceEvent> events;
        events.reserve(records.size());
        std::uint64_t sequence = 0;
        for (const auto& source : records) {
            ExpandedGraphTraceEvent event;
            event.sequence = ++sequence;
            event.timestampMicros = source.event.timestampNanoseconds / 1'000;
            event.thread = source.thread;
            event.event = std::string(TraceEventName(source.event.event));
            const bool schedulerEvent = source.event.event >= AsyncStateGraphTraceEventID::SchedulerTaskQueued &&
                source.event.event <= AsyncStateGraphTraceEventID::SchedulerTaskResubmitted;
            event.subsystem = schedulerEvent ? "TaskScheduler" : "AsyncStateGraph";
            event.key = source.event.key;
            event.revision = source.event.revision;
            event.generation = source.event.generation;
            event.related = source.event.related;
            event.relatedRevision = source.event.relatedRevision;
            event.readiness = source.event.readiness;
            event.durationMicros = source.event.durationMicros;
            event.detail = TraceDetail(source.event);
            events.push_back(std::move(event));
        }
        std::filesystem::create_directories(directory);
        AsyncStateGraphTraceReport report;
        report.eventsCsv = directory / "async_state_graph_events.csv";
        report.staticGroupCsv = directory / "async_state_graph_static_groups.csv";
        report.chromeTraceJson = directory / "async_state_graph_trace.json";
        report.summaryMarkdown = directory / "async_state_graph_summary.md";
        report.capturedEvents = events.size();
        report.droppedEvents = dropped;
        report.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_started);

        std::ofstream csv(report.eventsCsv, std::ios::trunc);
        csv << "sequence,timestamp_us,thread,subsystem,event,kind,primary_id,variant_id,revision,generation,"
               "readiness,duration_us,related_kind,related_primary_id,related_variant_id,related_revision,detail\n";
        for (const auto& event : events) {
            csv << event.sequence << ',' << event.timestampMicros << ',' << event.thread << ','
                << CsvField(event.subsystem) << ',' << CsvField(event.event) << ','
                << static_cast<unsigned>(event.key.kind) << ','
                << event.key.primaryID << ',' << event.key.variantID << ',' << event.revision << ','
                << event.generation << ',' << static_cast<unsigned>(event.readiness) << ','
                << event.durationMicros << ',' << static_cast<unsigned>(event.related.kind) << ','
                << event.related.primaryID << ',' << event.related.variantID << ','
                << event.relatedRevision << ',' << CsvField(event.detail) << '\n';
        }

        std::ofstream chrome(report.chromeTraceJson, std::ios::trunc);
        chrome << "{\"traceEvents\":[";
        bool first = true;
        for (const auto& event : events) {
            if (!first) chrome << ',';
            first = false;
            chrome << "{\"name\":\"" << JsonString(event.event) << "\",\"cat\":\""
                << JsonString(event.subsystem) << "\","
                << "\"ph\":\"" << (event.durationMicros > 0 ? "X" : "i") << "\",\"s\":\"t\","
                << "\"ts\":" << event.timestampMicros << ",\"dur\":" << event.durationMicros
                << ",\"pid\":1,\"tid\":" << event.thread << ",\"args\":{"
                << "\"artifact\":\"" << JsonString(KeyString(event.key)) << "\","
                << "\"artifact_kind\":\"" << KindName(event.key.kind) << "\","
                << "\"revision\":" << event.revision << ",\"generation\":" << event.generation
                << ",\"readiness\":\"" << ReadinessName(event.readiness) << "\""
                << ",\"related_artifact\":\"" << JsonString(KeyString(event.related)) << "\""
                << ",\"related_revision\":" << event.relatedRevision
                << ",\"detail\":\"" << JsonString(event.detail) << "\"}}";
        }
        chrome << "],\"displayTimeUnit\":\"ms\"}";

        struct Aggregate { std::uint64_t count = 0; std::int64_t total = 0; std::int64_t maximum = 0; };
        std::map<std::string, Aggregate> byEvent;
        std::map<unsigned, Aggregate> buildsByKind;
        std::map<unsigned, Aggregate> stateResidence;
        std::map<std::pair<unsigned, unsigned>, std::uint64_t> blockerEdges;
        using TraceVersion = std::tuple<unsigned, std::uint64_t, std::uint64_t, std::uint64_t>;
        using TraceAddressRevision = std::tuple<unsigned, std::uint64_t, std::uint64_t, std::uint64_t>;
        struct GroupStages {
            std::int64_t discovered = -1;
            std::int64_t workerSubmitted = -1;
            std::int64_t prepared = -1;
			std::int64_t batchQueued = -1;
			std::int64_t materialized = -1;
			std::int64_t validated = -1;
            std::int64_t bridgeApplied = -1;
            std::uint64_t sourceGeneration = 0;
        };
        struct GroupJourney {
            std::uint64_t groupID = 0;
            GroupStages stages;
            TraceVersion transaction{};
            std::int64_t linked = -1;
            std::string detail;
        };
        std::map<TraceVersion, std::pair<ArtifactReadiness, std::int64_t>> lastState;
        std::map<TraceVersion, std::map<ArtifactReadiness, std::int64_t>> stateTimes;
        std::map<TraceVersion, std::int64_t> manifestCommitTimes;
        std::map<ArtifactKey, std::uint64_t> lastCommittedRevision;
        std::vector<std::tuple<const ExpandedGraphTraceEvent*, std::uint64_t>> fragmentRegressions;
        std::map<TraceVersion, const ExpandedGraphTraceEvent*> submittedBuilds;
        std::map<std::uint64_t, GroupStages> currentGroupStages;
        std::map<TraceAddressRevision, std::vector<TraceVersion>> transactionScenes;
        std::map<TraceAddressRevision, std::vector<TraceAddressRevision>> transactionPages;
        std::map<TraceAddressRevision, std::vector<TraceVersion>> pageScenes;
        std::vector<GroupJourney> groupJourneys;
        std::vector<const ExpandedGraphTraceEvent*> slowBuilds;
        struct SchedulerAggregate {
            std::uint64_t queued = 0, admitted = 0, started = 0, completed = 0;
            std::uint64_t cancelled = 0, rejected = 0, resubmitted = 0;
            std::uint64_t peakQueued = 0, peakActive = 0;
            std::vector<std::int64_t> queueWaits;
            std::vector<std::int64_t> executions;
        };
        std::map<unsigned, SchedulerAggregate> schedulerByDomain;
        std::unordered_set<std::uint64_t> schedulerStartedTasks;
        std::unordered_set<std::uint64_t> schedulerTerminalTasks;
        std::unordered_set<std::uint64_t> graphProducerCorrelations;
        std::unordered_set<std::uint64_t> schedulerProducerCorrelations;
        for (const auto& event : events) {
            auto& aggregate = byEvent[event.event];
            ++aggregate.count;
            aggregate.total += event.durationMicros;
            aggregate.maximum = (std::max)(aggregate.maximum, event.durationMicros);
            if (event.subsystem == "TaskScheduler") {
                const auto domain = static_cast<unsigned>(event.detail.empty()
                    ? 0u : records[event.sequence - 1].event.payload.values[1]);
                auto& scheduler = schedulerByDomain[domain];
                const auto& raw = records[event.sequence - 1].event;
                scheduler.peakQueued = (std::max)(scheduler.peakQueued, raw.payload.values[7]);
                scheduler.peakActive = (std::max)(scheduler.peakActive, raw.related.primaryID);
                if (event.event == "SchedulerTaskQueued") ++scheduler.queued;
                else if (event.event == "SchedulerTaskAdmitted") ++scheduler.admitted;
                else if (event.event == "SchedulerTaskStarted") {
                    ++scheduler.started;
                    scheduler.queueWaits.push_back(static_cast<std::int64_t>(raw.related.variantID));
                    schedulerStartedTasks.insert(event.revision);
                } else if (event.event == "SchedulerTaskCompleted") {
                    ++scheduler.completed;
                    scheduler.executions.push_back(event.durationMicros);
                    schedulerTerminalTasks.insert(event.revision);
                } else if (event.event == "SchedulerTaskCancelled") {
                    ++scheduler.cancelled;
                    schedulerTerminalTasks.insert(event.revision);
                } else if (event.event == "SchedulerTaskRejected") ++scheduler.rejected;
                else if (event.event == "SchedulerTaskResubmitted") ++scheduler.resubmitted;
                if (event.event == "SchedulerTaskStarted" && event.generation != 0)
                    schedulerProducerCorrelations.insert(event.generation);
            }
            if (event.event == "BuildCompleted") {
                auto& kind = buildsByKind[static_cast<unsigned>(event.key.kind)];
                ++kind.count;
                kind.total += event.durationMicros;
                kind.maximum = (std::max)(kind.maximum, event.durationMicros);
                slowBuilds.push_back(&event);
            }
            if (event.event == "BuildStarted") {
                const auto correlation = records[event.sequence - 1].event.payload.values[1];
                if (correlation != 0) graphProducerCorrelations.insert(correlation);
            }
            if (event.event == "BuildSubmitted") {
                submittedBuilds[{ static_cast<unsigned>(event.key.kind), event.key.primaryID,
                    event.revision, event.generation }] = &event;
            } else if (event.event == "BuildStarted" || event.event == "BuildRejected") {
                submittedBuilds.erase({ static_cast<unsigned>(event.key.kind), event.key.primaryID,
                    event.revision, event.generation });
            }
            if (event.event == "ManifestFragmentCommitted") {
                manifestCommitTimes.try_emplace({ static_cast<unsigned>(event.key.kind),
                    event.key.primaryID, event.revision, event.generation }, event.timestampMicros);
                auto& previous = lastCommittedRevision[event.key];
                if (previous != 0 && event.revision < previous) {
                    fragmentRegressions.emplace_back(&event, previous);
                }
                previous = (std::max)(previous, event.revision);
            }
            if (event.event == "StateChanged") {
                const TraceVersion version{ static_cast<unsigned>(event.key.kind),
                    event.key.primaryID, event.revision, event.generation };
                stateTimes[version].try_emplace(event.readiness, event.timestampMicros);
                if (const auto previous = lastState.find(version); previous != lastState.end()) {
                    const auto duration = event.timestampMicros - previous->second.second;
                    auto& state = stateResidence[static_cast<unsigned>(previous->second.first)];
                    ++state.count;
                    state.total += duration;
                    state.maximum = (std::max)(state.maximum, duration);
                }
                lastState[version] = { event.readiness, event.timestampMicros };
            } else if (event.event == "VersionReclaimed") {
                lastState.erase({ static_cast<unsigned>(event.key.kind), event.key.primaryID,
                    event.revision, event.generation });
            }
            if (event.event == "DependencyBlocked") {
                ++blockerEdges[{ static_cast<unsigned>(event.key.kind),
                    static_cast<unsigned>(event.related.kind) }];
            }
            if (event.key.kind == ArtifactKind::StaticGroup) {
                auto& stages = currentGroupStages[event.key.primaryID];
                stages.sourceGeneration = event.revision;
                if (event.event == "StaticGroupDiscovered") stages.discovered = event.timestampMicros;
                else if (event.event == "StaticGroupWorkerSubmitted") stages.workerSubmitted = event.timestampMicros;
                else if (event.event == "StaticGroupPrepared") stages.prepared = event.timestampMicros;
				else if (event.event == "StaticGroupBatchQueued") stages.batchQueued = event.timestampMicros;
				else if (event.event == "StaticGroupMaterialized") stages.materialized = event.timestampMicros;
				else if (event.event == "StaticGroupValidated") stages.validated = event.timestampMicros;
                else if (event.event == "StaticGroupBridgeApplied") stages.bridgeApplied = event.timestampMicros;
            } else if (event.event == "StaticGroupTransactionLinked" &&
                event.related.kind == ArtifactKind::StaticGroup) {
                groupJourneys.push_back({ event.related.primaryID,
                    currentGroupStages[event.related.primaryID],
                    { static_cast<unsigned>(event.key.kind), event.key.primaryID,
                        event.revision, event.generation }, event.timestampMicros, event.detail });
            } else if (event.event == "DependencyDeclared") {
                const TraceAddressRevision related{
                    static_cast<unsigned>(event.related.kind), event.related.primaryID,
                    event.related.variantID, event.relatedRevision };
                if (event.key.kind == ArtifactKind::StaticScene &&
                    event.related.kind == ArtifactKind::StaticTransaction) {
                    transactionScenes[related].push_back({ static_cast<unsigned>(event.key.kind),
                        event.key.primaryID, event.revision, event.generation });
                } else if (event.key.kind == ArtifactKind::StaticScenePage &&
                    event.related.kind == ArtifactKind::StaticTransaction) {
                    transactionPages[related].push_back({ static_cast<unsigned>(event.key.kind),
                        event.key.primaryID, event.key.variantID, event.revision });
                } else if (event.key.kind == ArtifactKind::StaticScene &&
                    event.related.kind == ArtifactKind::StaticScenePage) {
                    pageScenes[related].push_back({ static_cast<unsigned>(event.key.kind),
                        event.key.primaryID, event.revision, event.generation });
                }
            }
        }
        for (const auto& [transaction, pages] : transactionPages) {
            auto& scenes = transactionScenes[transaction];
            for (const auto& page : pages) {
                const auto found = pageScenes.find(page);
                if (found == pageScenes.end()) continue;
                scenes.insert(scenes.end(), found->second.begin(), found->second.end());
            }
            std::ranges::sort(scenes);
            scenes.erase(std::unique(scenes.begin(), scenes.end()), scenes.end());
        }
        for (const auto& [_, state] : lastState) {
            const auto duration = report.elapsed.count() - state.second;
            auto& residence = stateResidence[static_cast<unsigned>(state.first)];
            ++residence.count;
            residence.total += duration;
            residence.maximum = (std::max)(residence.maximum, duration);
        }

        std::ofstream groups(report.staticGroupCsv, std::ios::trunc);
        groups << "group_id,source_generation,transaction_id,transaction_revision,transaction_generation,"
			"discovered_us,worker_submitted_us,prepared_us,batch_queued_us,materialized_us,validated_us,bridge_applied_us,graph_linked_us,"
            "transaction_cpu_ready_us,transaction_submitted_us,transaction_gpu_ready_us,"
            "transaction_published_us,static_scene_published_us,source_to_bridge_us,"
            "bridge_to_graph_us,graph_to_scene_published_us,end_to_end_us,detail\n";
        std::vector<std::int64_t> endToEndLatencies;
        std::vector<std::int64_t> graphLatencies;
        std::vector<std::int64_t> discoveryToWorkerLatencies;
        std::vector<std::int64_t> workerToPreparedLatencies;
        std::vector<std::int64_t> preparedToBridgeLatencies;
		std::vector<std::int64_t> preparedToBatchLatencies;
		std::vector<std::int64_t> batchToMaterializedLatencies;
		std::vector<std::int64_t> materializedToValidatedLatencies;
		std::vector<std::int64_t> validatedToBridgeLatencies;
		std::vector<std::int64_t> materializedToBridgeLatencies;
        std::vector<std::int64_t> linkToTransactionReadyLatencies;
        std::uint64_t completeGroupJourneys = 0;
        const auto timeFor = [&stateTimes](const TraceVersion& version, ArtifactReadiness readiness) {
            const auto versionIt = stateTimes.find(version);
            if (versionIt == stateTimes.end()) return std::int64_t{ -1 };
            const auto stateIt = versionIt->second.find(readiness);
            return stateIt == versionIt->second.end() ? std::int64_t{ -1 } : stateIt->second;
        };
        for (const auto& journey : groupJourneys) {
            const auto cpuReady = timeFor(journey.transaction, ArtifactReadiness::CpuReady);
            const auto submitted = timeFor(journey.transaction, ArtifactReadiness::UploadSubmitted);
            const auto gpuReady = timeFor(journey.transaction, ArtifactReadiness::GpuReady);
            const auto transactionPublished = timeFor(journey.transaction, ArtifactReadiness::Published);
            const TraceAddressRevision transactionAddress{
                std::get<0>(journey.transaction), std::get<1>(journey.transaction), 0,
                std::get<2>(journey.transaction) };
            std::int64_t scenePublished = -1;
            if (const auto found = transactionScenes.find(transactionAddress);
                found != transactionScenes.end()) {
                for (const auto& scene : found->second) {
                    const auto committed = manifestCommitTimes.find(scene);
                    const auto published = committed != manifestCommitTimes.end()
                        ? committed->second : timeFor(scene, ArtifactReadiness::Published);
                    if (published >= journey.linked &&
                        (scenePublished < 0 || published < scenePublished)) scenePublished = published;
                }
            }
            const auto sourceToBridge = journey.stages.discovered >= 0 && journey.stages.bridgeApplied >= 0
                ? journey.stages.bridgeApplied - journey.stages.discovered : -1;
            const auto bridgeToGraph = journey.stages.bridgeApplied >= 0
                ? journey.linked - journey.stages.bridgeApplied : -1;
            const auto graphToScene = scenePublished >= 0 ? scenePublished - journey.linked : -1;
            const auto endToEnd = journey.stages.discovered >= 0 && scenePublished >= 0
                ? scenePublished - journey.stages.discovered : -1;
            if (journey.stages.discovered >= 0 && journey.stages.workerSubmitted >= 0)
                discoveryToWorkerLatencies.push_back(
                    journey.stages.workerSubmitted - journey.stages.discovered);
            if (journey.stages.workerSubmitted >= 0 && journey.stages.prepared >= 0)
                workerToPreparedLatencies.push_back(
                    journey.stages.prepared - journey.stages.workerSubmitted);
            if (journey.stages.prepared >= 0 && journey.stages.bridgeApplied >= 0)
                preparedToBridgeLatencies.push_back(
                    journey.stages.bridgeApplied - journey.stages.prepared);
			if (journey.stages.prepared >= 0 && journey.stages.batchQueued >= 0)
				preparedToBatchLatencies.push_back(journey.stages.batchQueued - journey.stages.prepared);
			if (journey.stages.batchQueued >= 0 && journey.stages.materialized >= 0)
				batchToMaterializedLatencies.push_back(journey.stages.materialized - journey.stages.batchQueued);
			if (journey.stages.materialized >= 0 && journey.stages.bridgeApplied >= 0)
				materializedToBridgeLatencies.push_back(journey.stages.bridgeApplied - journey.stages.materialized);
			if (journey.stages.materialized >= 0 && journey.stages.validated >= 0)
				materializedToValidatedLatencies.push_back(journey.stages.validated - journey.stages.materialized);
			if (journey.stages.validated >= 0 && journey.stages.bridgeApplied >= 0)
				validatedToBridgeLatencies.push_back(journey.stages.bridgeApplied - journey.stages.validated);
            if (gpuReady >= journey.linked)
                linkToTransactionReadyLatencies.push_back(gpuReady - journey.linked);
            if (graphToScene >= 0) graphLatencies.push_back(graphToScene);
            if (endToEnd >= 0) {
                endToEndLatencies.push_back(endToEnd);
                ++completeGroupJourneys;
            }
            groups << journey.groupID << ',' << journey.stages.sourceGeneration << ','
                << std::get<1>(journey.transaction) << ',' << std::get<2>(journey.transaction) << ','
                << std::get<3>(journey.transaction) << ',' << journey.stages.discovered << ','
                << journey.stages.workerSubmitted << ',' << journey.stages.prepared << ','
				<< journey.stages.batchQueued << ',' << journey.stages.materialized << ','
				<< journey.stages.validated << ','
                << journey.stages.bridgeApplied << ',' << journey.linked << ',' << cpuReady << ','
                << submitted << ',' << gpuReady << ',' << transactionPublished << ','
                << scenePublished << ',' << sourceToBridge << ',' << bridgeToGraph << ','
                << graphToScene << ',' << endToEnd << ',' << CsvField(journey.detail) << '\n';
        }
        const auto percentile = [](std::vector<std::int64_t> values, double fraction) {
            if (values.empty()) return std::int64_t{ -1 };
            std::ranges::sort(values);
            const auto index = (std::min)(values.size() - 1,
                static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1)));
            return values[index];
        };
        std::ranges::sort(slowBuilds, std::greater{}, [](const ExpandedGraphTraceEvent* event) {
            return event->durationMicros;
        });
        std::ofstream summary(report.summaryMarkdown, std::ios::trunc);
        summary << "# Async State Graph Trace\n\n"
            << "- Elapsed: " << report.elapsed.count() << " us\n"
            << "- Captured events: " << report.capturedEvents << "\n"
            << "- Dropped events: " << report.droppedEvents << "\n"
            << "- Producer threads: " << shards.size() << "\n"
            << "- Fixed chunks: " << chunkCount << "\n"
            << "- POD record bytes: " << sizeof(GraphTraceEvent) << "\n\n"
            << "## Event totals\n\n| Event | Count | Total duration (us) | Maximum (us) |\n"
            << "|---|---:|---:|---:|\n";
        for (const auto& [name, aggregate] : byEvent) {
            summary << "| " << name << " | " << aggregate.count << " | " << aggregate.total
                << " | " << aggregate.maximum << " |\n";
        }
        summary << "\n## Unified scheduler execution\n\n"
            << "Scheduler events share this trace clock and fixed POD storage with graph events. "
               "Domain numbers use `TaskDomain` numeric identities.\n\n"
            << "| Domain | Queued | Admitted | Started | Completed | Cancelled | Rejected | "
               "Resubmitted | Peak queued | Peak active | Queue p50/p95/p99/max (us) | "
               "Execution p50/p95/p99/max (us) |\n"
            << "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|\n";
        for (const auto& [domain, scheduler] : schedulerByDomain) {
            summary << "| " << domain << " | " << scheduler.queued << " | "
                << scheduler.admitted << " | " << scheduler.started << " | "
                << scheduler.completed << " | " << scheduler.cancelled << " | "
                << scheduler.rejected << " | " << scheduler.resubmitted << " | "
                << scheduler.peakQueued << " | " << scheduler.peakActive << " | "
                << percentile(scheduler.queueWaits, 0.50) << '/'
                << percentile(scheduler.queueWaits, 0.95) << '/'
                << percentile(scheduler.queueWaits, 0.99) << '/'
                << percentile(scheduler.queueWaits, 1.0) << " | "
                << percentile(scheduler.executions, 0.50) << '/'
                << percentile(scheduler.executions, 0.95) << '/'
                << percentile(scheduler.executions, 0.99) << '/'
                << percentile(scheduler.executions, 1.0) << " |\n";
        }
        std::uint64_t startedWithoutTerminal = 0;
        for (const auto task : schedulerStartedTasks) {
            if (!schedulerTerminalTasks.contains(task)) ++startedWithoutTerminal;
        }
        summary << "\n- Started tasks without a terminal event at trace stop: "
            << startedWithoutTerminal << "\n";
        std::uint64_t graphProducersWithoutSchedulerTask = 0;
        for (const auto correlation : graphProducerCorrelations) {
            if (!schedulerProducerCorrelations.contains(correlation))
                ++graphProducersWithoutSchedulerTask;
        }
        summary << "- Graph producer correlations without a scheduler start: "
            << graphProducersWithoutSchedulerTask << "\n";
        summary << "- Scheduler-correlated graph producers: "
            << schedulerProducerCorrelations.size() << "\n";
        summary << "\n### Scheduler cumulative timing\n\n"
            << "| Domain | Queue wait total (us) | Execution total (us) |\n"
            << "|---:|---:|---:|\n";
        for (const auto& [domain, scheduler] : schedulerByDomain) {
            const auto queueTotal = std::accumulate(scheduler.queueWaits.begin(),
                scheduler.queueWaits.end(), std::int64_t{ 0 });
            const auto executionTotal = std::accumulate(scheduler.executions.begin(),
                scheduler.executions.end(), std::int64_t{ 0 });
            summary << "| " << domain << " | " << queueTotal << " | "
                << executionTotal << " |\n";
        }
        summary << "\n## Graph mutex timing\n\n"
            << "Counts and timing are accumulated in thread-local storage. Graph containers are not "
               "inspected inside these timed lock sections. Slow samples (wait or hold >= 2,000 us) "
               "are also emitted as GraphMutex events.\n\n"
			<< "| Phase | Count | Total wait (us) | Max wait (us) | Total hold (us) | Max hold (us) | Total hold CPU (us) | Max hold CPU (us) |\n"
			<< "|---|---:|---:|---:|---:|---:|---:|---:|\n";
        for (std::size_t index = 0; index < mutexAggregates.size(); ++index) {
            const auto& aggregate = mutexAggregates[index];
            if (aggregate.count == 0) continue;
            summary << "| " << GraphMutexPhaseName(static_cast<GraphMutexPhase>(index))
                << " | " << aggregate.count << " | " << aggregate.totalWaitMicros
                << " | " << aggregate.maximumWaitMicros << " | "
                << aggregate.totalHoldMicros << " | " << aggregate.maximumHoldMicros
				<< " | " << aggregate.totalHoldCpuMicros << " | " << aggregate.maximumHoldCpuMicros
                << " |\n";
        }
        summary << "\n## Graph population snapshots\n\n"
            << "Population is sampled periodically through a single mutation-side event, independently "
               "of mutex timing.\n\n"
            << "- Snapshots: " << populationSnapshotCount << "\n"
            << "- Latest nodes/versions/pending/completions/GPU recovery/waiters: "
            << latestPopulation.nodes << '/' << latestPopulation.versions << '/'
            << latestPopulation.pending << '/' << latestPopulation.completions << '/'
            << latestPopulation.gpuRecovery << '/' << latestPopulation.waiters << "\n"
            << "- Maximum nodes/versions/pending/completions/GPU recovery/waiters: "
            << maximumPopulation.nodes << '/' << maximumPopulation.versions << '/'
            << maximumPopulation.pending << '/' << maximumPopulation.completions << '/'
            << maximumPopulation.gpuRecovery << '/' << maximumPopulation.waiters << "\n";
        summary << "\n## Producer timing by artifact kind\n\n"
            << "| Kind | Builds | Total (us) | Average (us) | Maximum (us) |\n"
            << "|---:|---:|---:|---:|---:|\n";
        for (const auto& [kind, aggregate] : buildsByKind) {
            summary << "| " << KindName(static_cast<ArtifactKind>(kind)) << " | "
                << aggregate.count << " | " << aggregate.total
                << " | " << (aggregate.count ? aggregate.total / static_cast<std::int64_t>(aggregate.count) : 0)
                << " | " << aggregate.maximum << " |\n";
        }
        summary << "\n## State residence\n\n"
            << "Open intervals are charged through trace stop.\n\n"
            << "| Readiness | Intervals | Total (us) | Maximum (us) |\n"
            << "|---:|---:|---:|---:|\n";
        for (const auto& [state, aggregate] : stateResidence) {
            summary << "| " << ReadinessName(static_cast<ArtifactReadiness>(state)) << " | "
                << aggregate.count << " | "
                << aggregate.total << " | " << aggregate.maximum << " |\n";
        }
        summary << "\n## Dependency blockers\n\n"
            << "| Consumer kind | Dependency kind | Blocked observations |\n"
            << "|---|---|---:|\n";
        for (const auto& [edge, count] : blockerEdges) {
            summary << "| " << KindName(static_cast<ArtifactKind>(edge.first)) << " | "
                << KindName(static_cast<ArtifactKind>(edge.second)) << " | " << count << " |\n";
        }
        summary << "\n## Static group end-to-end latency\n\n"
            << "- Linked group versions: " << groupJourneys.size() << "\n"
            << "- Complete discovery-to-static-scene-publication journeys: "
            << completeGroupJourneys << "\n"
            << "- Graph link-to-static-scene publication p50/p95/p99/max: "
            << percentile(graphLatencies, 0.50) << " / " << percentile(graphLatencies, 0.95)
            << " / " << percentile(graphLatencies, 0.99) << " / "
            << percentile(graphLatencies, 1.0) << " us\n"
            << "- Discovery-to-static-scene publication p50/p95/p99/max: "
            << percentile(endToEndLatencies, 0.50) << " / " << percentile(endToEndLatencies, 0.95)
            << " / " << percentile(endToEndLatencies, 0.99) << " / "
            << percentile(endToEndLatencies, 1.0) << " us\n";
        const auto writeStage = [&summary, &percentile](std::string_view label,
            const std::vector<std::int64_t>& values) {
            summary << "- " << label << " count/p50/p95/p99/max: " << values.size() << " / "
                << percentile(values, 0.50) << " / " << percentile(values, 0.95) << " / "
                << percentile(values, 0.99) << " / " << percentile(values, 1.0) << " us\n";
        };
        writeStage("Discovery-to-worker-submit", discoveryToWorkerLatencies);
        writeStage("Worker-submit-to-prepared", workerToPreparedLatencies);
		writeStage("Prepared-to-batch-queue", preparedToBatchLatencies);
		writeStage("Batch-queue-to-materialized", batchToMaterializedLatencies);
		writeStage("Materialized-to-validated", materializedToValidatedLatencies);
		writeStage("Validated-to-bridge-apply", validatedToBridgeLatencies);
		writeStage("Materialized-to-bridge-apply", materializedToBridgeLatencies);
        writeStage("Prepared-to-bridge-apply", preparedToBridgeLatencies);
        writeStage("Graph-link-to-transaction-GPU-ready", linkToTransactionReadyLatencies);

        summary << "\n## Manifest fragment regressions\n\n"
            << "- Regression events: " << fragmentRegressions.size() << "\n\n"
            << "| Timestamp (us) | Artifact | Previous revision | Selected revision | Generation | Detail |\n"
            << "|---:|---|---:|---:|---:|---|\n";
        for (std::size_t index = 0;
            index < (std::min<std::size_t>)(fragmentRegressions.size(), 25); ++index) {
            const auto& [event, previous] = fragmentRegressions[index];
            summary << "| " << event->timestampMicros << " | " << KindName(event->key.kind)
                << ':' << event->key.primaryID << ':' << event->key.variantID << " | "
                << previous << " | " << event->revision << " | " << event->generation
                << " | " << event->detail << " |\n";
        }

        summary << "\n## Scheduled producers not started at trace stop\n\n"
            << "| Artifact | Revision | Generation | Queue age (us) | Task |\n"
            << "|---|---:|---:|---:|---|\n";
        std::vector<const ExpandedGraphTraceEvent*> pendingBuilds;
        for (const auto& [_, event] : submittedBuilds) pendingBuilds.push_back(event);
        std::ranges::sort(pendingBuilds, {}, &ExpandedGraphTraceEvent::timestampMicros);
        for (std::size_t index = 0; index < (std::min<std::size_t>)(pendingBuilds.size(), 25); ++index) {
            const auto& event = *pendingBuilds[index];
            summary << "| " << KindName(event.key.kind) << ':' << event.key.primaryID << ':'
                << event.key.variantID << " | " << event.revision << " | " << event.generation
                << " | " << report.elapsed.count() - event.timestampMicros << " | "
                << event.detail << " |\n";
        }
        summary << "\n## Oldest unresolved artifact versions\n\n"
            << "| Artifact | Revision | Generation | State | State age (us) |\n"
            << "|---|---:|---:|---|---:|\n";
        std::vector<std::pair<TraceVersion, std::pair<ArtifactReadiness, std::int64_t>>> unresolved;
        for (const auto& state : lastState) {
            if (state.second.first == ArtifactReadiness::GpuReady ||
                state.second.first == ArtifactReadiness::Published ||
                state.second.first == ArtifactReadiness::Superseded ||
                state.second.first == ArtifactReadiness::Cancelled ||
                state.second.first == ArtifactReadiness::Failed) continue;
            unresolved.push_back(state);
        }
        std::ranges::sort(unresolved, {}, [](const auto& value) { return value.second.second; });
        for (std::size_t index = 0; index < (std::min<std::size_t>)(unresolved.size(), 25); ++index) {
            const auto& [version, state] = unresolved[index];
            summary << "| " << KindName(static_cast<ArtifactKind>(std::get<0>(version))) << ':'
                << std::get<1>(version) << " | " << std::get<2>(version) << " | "
                << std::get<3>(version) << " | " << ReadinessName(state.first) << " | "
                << report.elapsed.count() - state.second << " |\n";
        }
        summary << "\n## Slowest producers\n\n"
            << "| Artifact | Revision | Generation | Duration (us) | Detail |\n"
            << "|---|---:|---:|---:|---|\n";
        for (std::size_t index = 0; index < (std::min<std::size_t>)(slowBuilds.size(), 25); ++index) {
            const auto& event = *slowBuilds[index];
            summary << "| " << KeyString(event.key) << " | " << event.revision << " | "
                << event.generation << " | " << event.durationMicros << " | "
                << event.detail << " |\n";
        }
        return report;
    }

    const AsyncStateGraphTraceConfig& Config() const { return m_config; }

private:
    TraceShard* ThreadShard() {
        struct CacheEntry { const GraphTraceSession* session = nullptr; std::uint64_t id = 0; TraceShard* shard = nullptr; };
        thread_local CacheEntry cache;
        if (cache.session == this && cache.id == m_sessionID) return cache.shard;
        auto shard = std::make_shared<TraceShard>();
        shard->thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        auto* result = shard.get();
        m_shards.push(shard);
        cache = { this, m_sessionID, result };
        return result;
    }

    AsyncStateGraphTraceConfig m_config;
    std::chrono::steady_clock::time_point m_started;
    tbb::concurrent_queue<std::shared_ptr<TraceShard>> m_shards;
    std::atomic_size_t m_reservedEvents{ 0 };
    inline static std::atomic_uint64_t s_nextSessionID{ 0 };
    std::uint64_t m_sessionID{ s_nextSessionID.fetch_add(1, std::memory_order_relaxed) + 1 };
};

} // namespace

std::size_t ArtifactAddress::Hasher::operator()(const ArtifactAddress& key) const noexcept {
    auto value = static_cast<std::size_t>(key.kind);
    value ^= std::hash<std::uint64_t>{}(key.primaryID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    value ^= std::hash<std::uint64_t>{}(key.variantID) + 0x9e3779b9u + (value << 6u) + (value >> 2u);
    return value;
}

ArtifactBuildResult ArtifactBuildResult::Ready(ArtifactPayload payload,
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Ready;
    result.payload = std::move(payload);
    result.gpuSubmissions = std::move(gpuSubmissions);
    return result;
}

ArtifactSuspension ArtifactSuspension::Exact(ArtifactVersionID dependencyValue,
    ArtifactReadiness milestoneValue) {
    ArtifactSuspension suspension;
    suspension.kind = ArtifactSuspensionKind::ExactDependency;
    suspension.dependency = dependencyValue;
    suspension.milestone = milestoneValue;
    return suspension;
}

ArtifactSuspension ArtifactSuspension::Capacity(std::uint64_t identityValue,
    std::string reasonValue) {
    ArtifactSuspension suspension;
    suspension.kind = ArtifactSuspensionKind::Capacity;
    suspension.identity = identityValue;
    suspension.reason = std::move(reasonValue);
    return suspension;
}

ArtifactSuspension ArtifactSuspension::External(std::uint64_t identityValue,
    std::string reasonValue) {
    ArtifactSuspension suspension;
    suspension.kind = ArtifactSuspensionKind::ExternalOperation;
    suspension.identity = identityValue;
    suspension.reason = std::move(reasonValue);
    return suspension;
}

ArtifactSuspension ArtifactSuspension::Transient(std::uint64_t identityValue,
    std::chrono::steady_clock::time_point deadlineValue, std::uint32_t maximumAttemptsValue,
    std::string reasonValue) {
    ArtifactSuspension suspension;
    suspension.kind = ArtifactSuspensionKind::TransientRetry;
    suspension.identity = identityValue;
    suspension.deadline = deadlineValue;
    suspension.maximumAttempts = maximumAttemptsValue;
    suspension.reason = std::move(reasonValue);
    return suspension;
}

ArtifactBuildResult ArtifactBuildResult::Needs(std::vector<ArtifactRequirement> requirements,
    ArtifactPayload checkpoint) {
    ArtifactBuildResult result;
    result.outcome = Outcome::NeedsDependencies;
    result.requirements = std::move(requirements);
    result.checkpoint = std::move(checkpoint);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Retry(std::chrono::steady_clock::duration delay,
    ArtifactPayload checkpoint) {
    ArtifactBuildResult result;
    result.outcome = Outcome::RetryAfter;
    result.retryDelay = delay;
    result.checkpoint = std::move(checkpoint);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Suspend(ArtifactSuspension suspension,
    ArtifactPayload checkpoint) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Suspended;
    result.suspension = std::move(suspension);
    result.checkpoint = std::move(checkpoint);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Failure(std::string error) {
    ArtifactBuildResult result;
    result.outcome = Outcome::Failed;
    result.error = std::move(error);
    return result;
}

ArtifactBuildResult ArtifactBuildResult::Cancelled() {
    ArtifactBuildResult result;
    result.outcome = Outcome::Cancelled;
    return result;
}

std::shared_ptr<const GpuSubmissionSet> MakeGpuSubmissionSet(
    const std::shared_ptr<org::TrackedUploadTicket>& ticket) {
    if (!ticket) return {};
    auto token = std::make_shared<GpuSubmissionSet>();
    GpuQueueSubmission submission;
    {
        std::lock_guard lock(ticket->timelineMutex);
        submission.timelineOwner = ticket->timelineOwner;
        submission.value = ticket->timelineValue;
    }
    token->isComplete = [ticket] { return ticket->Complete(); };
    token->isSubmitted = [ticket] {
        const auto state = ticket->state.load(std::memory_order_acquire);
        return state == org::TrackedUploadTicketState::Submitted ||
            state == org::TrackedUploadTicketState::Completed;
    };
    submission.currentTimelineOwner = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineOwner;
    };
    submission.currentValue = [ticket] {
        std::lock_guard lock(ticket->timelineMutex);
        return ticket->timelineValue;
    };
    token->submissions.push_back(std::move(submission));
    token->subscribe = [ticket](std::function<void()> callback) {
        ticket->SetChangeCallback(callback);
        // Atomically installing on the ticket and then reconciling graph state
        // makes the observation level-triggered across registration races.
        if (callback) callback();
    };
    token->completionNotificationsAreAuthoritative = true;
    token->cancel = [ticket] { return ticket->Cancel(); };
    return token;
}

struct AsyncStateGraph::Impl : std::enable_shared_from_this<Impl> {
    struct VersionKey {
        ArtifactKey address;
        std::uint64_t revision = 0;
        auto operator<=>(const VersionKey&) const = default;
        struct Hasher {
            std::size_t operator()(const VersionKey& value) const noexcept {
                auto hash = ArtifactKey::Hasher{}(value.address);
                hash ^= std::hash<std::uint64_t>{}(value.revision) + 0x9e3779b9u +
                    (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
    };

    struct StoredVersionKey {
        ArtifactKey address;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        auto operator<=>(const StoredVersionKey&) const = default;
        struct Hasher {
            std::size_t operator()(const StoredVersionKey& value) const noexcept {
                auto hash = VersionKey::Hasher{}({ value.address, value.revision });
                hash ^= std::hash<std::uint64_t>{}(value.generation) + 0x9e3779b9u +
                    (hash << 6u) + (hash >> 2u);
                return hash;
            }
        };
    };

    struct RequestedVersion {
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        std::uint64_t fingerprint = 0;
        ArtifactPayload input;
        std::vector<ArtifactRequirement> requirements;
        std::vector<ArtifactRequirement> requestedRequirements;
        ArtifactLease lease;
        bool coalescibleIntent = false;
    };

    struct VersionSignature {
        std::uint64_t fingerprint = 0;
        std::type_index inputType{ typeid(void) };
        std::vector<ArtifactRequirement> requirements;
    };

    struct Node {
        ArtifactKey key;
        std::uint64_t desiredRevision = 0;
        std::uint64_t latestRequestedRevision = 0;
        std::uint64_t producedRevision = 0;
        std::uint64_t generation = 0;
        std::uint64_t versionGeneration = 0;
        std::uint64_t requestFingerprint = 0;
        ArtifactLease lease;
        ArtifactReadiness state = ArtifactReadiness::Missing;
        ArtifactPayload payload;
        ArtifactPayload input;
        ArtifactPayload checkpoint;
        // Stable recipe supplied by Request. A multi-phase producer may replace
        // requirements with exact handles without losing the Latest edges that
        // define future successor versions.
        std::vector<ArtifactRequirement> requestedRequirements;
        std::vector<ArtifactRequirement> requirements;
        std::vector<ArtifactSnapshot> resolvedDependencies;
		// Large aggregate recipes are woken once per dependency transition. Keep
		// address indexes for those nodes so each wake does not rescan thousands
		// of requirements and captured Latest snapshots under the graph mutex.
		std::unordered_map<ArtifactKey, std::size_t, ArtifactKey::Hasher>
			requestedRequirementIndex;
		std::unordered_map<ArtifactKey, std::pair<std::uint64_t, std::uint64_t>,
			ArtifactKey::Hasher> resolvedDependencyIndex;
		// The reverse dependency index stores one edge per dependency address,
		// regardless of how many recipe requirements mention that address. Keep
		// the exact installed address set on the consumer so replacement and
		// completion pruning do not have to rediscover it with quadratic scans of
		// requestedRequirements/requirements.
		std::vector<ArtifactKey> installedWaiterKeys;
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
        std::shared_ptr<const GpuSubmissionSet> waitingGpuSubmissions;
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        std::optional<ArtifactSuspension> suspension;
        std::string error;
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
        bool buildInFlight = false;
        bool buildAttempted = false;
        bool desired = false;
        bool terminalFailure = false;
        bool published = false;
        bool latestSuccessorNeeded = false;
		bool completedWaiterEdgesPruned = false;
        // True when this logical version came from SubmitLatest and may be
        // replaced as a whole. Its internal exact resource recipe must not pin
        // obsolete mutable children against the next mailbox drain.
        bool coalescibleIntent = false;
        // Set when a coalescible successor makes the running build obsolete.
        // Producers can test this without taking the graph mutex.
        std::shared_ptr<std::atomic_bool> superseded = std::make_shared<std::atomic_bool>(false);
        std::chrono::steady_clock::time_point queuedAt{};
        std::chrono::steady_clock::time_point buildStartedAt{};
        std::chrono::steady_clock::time_point uploadSubmittedAt{};
        std::deque<RequestedVersion> successors;
		// Scratch owned by the graph mutex for allocation-free dependency-cycle
		// traversals. The epoch makes clearing all nodes unnecessary.
		std::uint64_t cycleVisitEpoch = 0;
		std::optional<ArtifactKey> cycleVisitParent;
    };

    struct Completion {
        ArtifactKey key;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        std::vector<ArtifactSnapshot> dependencies;
        ArtifactBuildResult result;
        std::chrono::steady_clock::time_point queuedAt = std::chrono::steady_clock::now();
		bool gpuStateEvaluated = false;
		bool gpuSubmitted = false;
		bool gpuComplete = false;
    };

	struct PendingGpuSubscription {
		ArtifactKey key;
		std::shared_ptr<const GpuSubmissionSet> token;
	};

    struct AcceptanceDispatch {
        Completion completion;
        ArtifactAcceptanceRegistration registration;
        ArtifactSnapshot snapshot;
        std::chrono::steady_clock::time_point queuedAt = std::chrono::steady_clock::now();
    };

    struct RetryEntry {
        std::chrono::steady_clock::time_point deadline{};
        ArtifactKey key{};
        std::uint64_t generation = 0;
        bool operator>(const RetryEntry& other) const noexcept {
            return deadline > other.deadline;
        }
    };

	struct GpuSignal {
		ArtifactKey key{};
		std::chrono::steady_clock::time_point queuedAt = std::chrono::steady_clock::now();
	};

    TaskSchedulerManager& scheduler;
    TaskScope scope;
    mutable std::mutex mutex;
    std::unordered_map<ArtifactKey, Node, ArtifactKey::Hasher> nodes;
    // Completed versions are immutable. The mutable address slot above is only
    // the desired/build cursor; ExactSnapshot never resolves through that cursor.
    std::unordered_map<StoredVersionKey, ArtifactSnapshot, StoredVersionKey::Hasher> versions;
    using AddressVersionIndex = std::map<std::pair<std::uint64_t, std::uint64_t>, StoredVersionKey>;
    std::unordered_map<ArtifactKey, AddressVersionIndex, ArtifactKey::Hasher> versionsByAddress;
    std::unordered_map<VersionKey, std::uint64_t, VersionKey::Hasher> versionGenerations;
    std::unordered_map<VersionKey, VersionSignature, VersionKey::Hasher> versionSignatures;
    mutable std::unordered_map<StoredVersionKey, std::weak_ptr<const void>, StoredVersionKey::Hasher> versionLeases;
    std::uint64_t nextVersionGeneration = 0;
	std::uint64_t nextCycleVisitEpoch = 0;
    std::uint64_t reclaimedVersions = 0;
    std::unordered_map<ArtifactKey, std::unordered_set<ArtifactKey, ArtifactKey::Hasher>, ArtifactKey::Hasher> waiters;
    // Exact immutable recipes pin versions by identity. Maintaining this index
    // at recipe admission/replacement keeps supersession checks O(1) and avoids
    // scanning the entire graph while holding its control mutex.
    std::unordered_map<StoredVersionKey, std::uint32_t, StoredVersionKey::Hasher> exactRecipePins;
    std::unordered_map<ArtifactKind, ArtifactProducerRegistration> producers;
    std::deque<ArtifactKey> pending;
	struct PendingWaiterWake {
		ArtifactKey dependency{};
		std::vector<ArtifactKey> consumers;
		std::size_t next = 0;
	};
	std::deque<PendingWaiterWake> pendingWaiterWakes;
	tbb::concurrent_queue<Completion> completions;
	std::atomic<std::uint64_t> completionCount{ 0 };
    static constexpr std::size_t kAcceptanceMailboxCount =
        static_cast<std::size_t>(TaskLane::Count) *
        static_cast<std::size_t>(TaskDomain::Count);
    std::array<std::deque<AcceptanceDispatch>, kAcceptanceMailboxCount> acceptanceMailboxes;
    std::array<bool, kAcceptanceMailboxCount> acceptanceMailboxScheduled{};
    std::unordered_map<std::uint64_t, StoredVersionKey> suspendedByIdentity;
    std::unordered_set<std::uint64_t> satisfiedSuspensions;
    std::deque<ArtifactSnapshot> pendingRetirement;
	tbb::concurrent_queue<StoredVersionKey> reclaimQueue;
	tbb::concurrent_queue<StoredVersionKey> publishedSignals;
    std::priority_queue<RetryEntry, std::vector<RetryEntry>, std::greater<>> retries;
	tbb::concurrent_queue<GpuSignal> gpuSignals;
    std::deque<ArtifactKey> gpuRecovery;
    std::unordered_map<std::uint64_t, std::function<void(const ArtifactSnapshot&)>> readyCallbacks;
    std::uint64_t nextReadyCallback = 0;
	struct ExactWaiter {
		std::uint64_t subscription = 0;
		ArtifactVersionID version;
		ArtifactReadiness milestone = ArtifactReadiness::Missing;
		TaskLane lane = TaskLane::Streaming;
		TaskDomain domain = TaskDomain::RendererState;
		std::function<void(const ArtifactSnapshot&)> continuation;
		ArtifactLease lease;
	};
	std::unordered_map<StoredVersionKey, std::vector<ExactWaiter>, StoredVersionKey::Hasher> exactWaiters;
	std::uint64_t exactWaiterCount = 0;
	std::array<std::uint64_t,
		static_cast<std::size_t>(ArtifactReadiness::Failed) + 1u> nodeStateCounts{};
	std::array<std::uint64_t, kArtifactKindCount> outstandingByKind{};
	std::uint64_t nextExactWaiter = 0;
    AsyncStateGraphStats stats;
    br::SerializedTaskPump drainPump;
    std::atomic_bool delayedDrainScheduled{ false };
    std::atomic_bool shuttingDown{ false };
    std::atomic_bool pauseGpuRecovery{ false };
    std::chrono::steady_clock::time_point nextDiagnosticSnapshot{};
    std::chrono::steady_clock::time_point nextTracePopulationSnapshot{};
    struct TraceHazardSlot {
        std::atomic<GraphTraceSession*> pointer{ nullptr };
        TraceHazardSlot* next = nullptr;
    };

    class TraceGuard {
    public:
        TraceGuard() = default;
        TraceGuard(GraphTraceSession* session, TraceHazardSlot* hazard) noexcept
            : m_session(session), m_hazard(hazard) {}
        ~TraceGuard() { Reset(); }
        TraceGuard(const TraceGuard&) = delete;
        TraceGuard& operator=(const TraceGuard&) = delete;
        TraceGuard(TraceGuard&& other) noexcept
            : m_session(std::exchange(other.m_session, nullptr)),
              m_hazard(std::exchange(other.m_hazard, nullptr)) {}
        TraceGuard& operator=(TraceGuard&& other) noexcept {
            if (this == &other) return *this;
            Reset();
            m_session = std::exchange(other.m_session, nullptr);
            m_hazard = std::exchange(other.m_hazard, nullptr);
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept { return m_session != nullptr; }
        [[nodiscard]] GraphTraceSession* operator->() const noexcept { return m_session; }

    private:
        void Reset() noexcept {
            if (m_hazard) m_hazard->pointer.store(nullptr, std::memory_order_seq_cst);
            m_session = nullptr;
            m_hazard = nullptr;
        }

        GraphTraceSession* m_session = nullptr;
        TraceHazardSlot* m_hazard = nullptr;
    };

    std::atomic<GraphTraceSession*> trace{ nullptr };
    std::shared_ptr<GraphTraceSession> traceOwner;
    std::atomic<TraceHazardSlot*> traceHazards{ nullptr };
    const std::uint64_t traceHazardIdentity = [] {
        static std::atomic_uint64_t next{ 1 };
        return next.fetch_add(1, std::memory_order_relaxed);
    }();

    [[nodiscard]] TraceHazardSlot* ThreadTraceHazard() {
        struct CacheEntry {
            const Impl* owner = nullptr;
            std::uint64_t identity = 0;
            TraceHazardSlot* slot = nullptr;
        };
        static thread_local std::vector<CacheEntry> cache;
        for (const auto& entry : cache) {
            if (entry.owner == this && entry.identity == traceHazardIdentity) return entry.slot;
        }

        auto* slot = new TraceHazardSlot;
        auto* head = traceHazards.load(std::memory_order_seq_cst);
        do {
            slot->next = head;
        } while (!traceHazards.compare_exchange_weak(head, slot,
            std::memory_order_seq_cst, std::memory_order_seq_cst));
        cache.push_back({ this, traceHazardIdentity, slot });
        return slot;
    }

    [[nodiscard]] TraceGuard AcquireTrace() {
        // The trace-off path is a single raw atomic load. A hazard slot is only
        // acquired after tracing is observed active, and is thread-local after
        // its first use, so recording never touches a shared reference count.
        auto* candidate = trace.load(std::memory_order_acquire);
        if (!candidate) return {};
        auto* hazard = ThreadTraceHazard();
        for (;;) {
            hazard->pointer.store(candidate, std::memory_order_seq_cst);
            auto* verified = trace.load(std::memory_order_seq_cst);
            if (candidate == verified) return TraceGuard(candidate, hazard);
            candidate = verified;
            if (!candidate) {
                hazard->pointer.store(nullptr, std::memory_order_seq_cst);
                return {};
            }
        }
    }

    class TimedMutexLock {
    public:
        TimedMutexLock(Impl& owner, GraphMutexPhase phase, bool acquire = true)
            : m_owner(&owner), m_phase(phase), m_lock(owner.mutex, std::defer_lock),
              m_trace(acquire ? owner.AcquireTrace() : TraceGuard{}) {
            if (!acquire) return;
            if (m_trace) m_waitStarted = std::chrono::steady_clock::now();
            m_lock.lock();
			if (m_trace) {
				m_acquired = std::chrono::steady_clock::now();
				m_acquiredCpuMicros = CurrentThreadCpuMicros();
			}
        }
        ~TimedMutexLock() { Unlock(); }
        TimedMutexLock(const TimedMutexLock&) = delete;
        TimedMutexLock& operator=(const TimedMutexLock&) = delete;

        void Unlock() {
            if (!m_lock.owns_lock()) return;
            if (!m_trace) {
                m_lock.unlock();
                return;
            }
            const auto released = std::chrono::steady_clock::now();
			const auto releasedCpuMicros = CurrentThreadCpuMicros();
            m_lock.unlock();
            const auto waitMicros = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    m_acquired - m_waitStarted).count());
            const auto holdMicros = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    released - m_acquired).count());
			const auto holdCpuMicros = releasedCpuMicros >= m_acquiredCpuMicros
				? releasedCpuMicros - m_acquiredCpuMicros : 0;
			m_trace->RecordMutex(m_phase, waitMicros, holdMicros, holdCpuMicros, {});
        }

    private:
        Impl* m_owner;
        GraphMutexPhase m_phase;
        std::chrono::steady_clock::time_point m_waitStarted;
        std::unique_lock<std::mutex> m_lock;
        std::chrono::steady_clock::time_point m_acquired;
		std::uint64_t m_acquiredCpuMicros = 0;
        TraceGuard m_trace;
    };

    [[nodiscard]] TimedMutexLock LockMutex(GraphMutexPhase phase) {
        return TimedMutexLock(*this, phase);
    }

    Impl(TaskSchedulerManager& schedulerIn, std::string_view name)
        : scheduler(schedulerIn), scope(schedulerIn.CreateScope(name)) {}

    ~Impl() {
        auto* hazard = traceHazards.load(std::memory_order_relaxed);
        while (hazard) {
            auto* next = hazard->next;
            delete hazard;
            hazard = next;
        }
    }

    static std::size_t AcceptanceMailboxIndex(TaskLane lane, TaskDomain domain) {
        return static_cast<std::size_t>(domain) * static_cast<std::size_t>(TaskLane::Count) +
            static_cast<std::size_t>(lane);
    }

    void ScheduleAcceptanceMailbox(std::size_t index) {
        const auto lane = static_cast<TaskLane>(index % static_cast<std::size_t>(TaskLane::Count));
        const auto domain = static_cast<TaskDomain>(index / static_cast<std::size_t>(TaskLane::Count));
        auto weak = weak_from_this();
        if (scheduler.Submit(scope, lane, domain, "AsyncStateGraph::AcceptanceMailbox",
            [weak, index](const TaskContext& context) {
                if (auto self = weak.lock()) self->DrainAcceptanceMailbox(index, context);
            })) return;
        auto lock = LockMutex(GraphMutexPhase::AcceptanceScheduleFailure);
        auto& mailbox = acceptanceMailboxes[index];
        while (!mailbox.empty()) {
            auto item = std::move(mailbox.front());
            mailbox.pop_front();
            item.completion.result = ArtifactBuildResult::Failure(
                "scheduler rejected acceptance mailbox");
            item.completion.queuedAt = std::chrono::steady_clock::now();
			completions.push(std::move(item.completion));
			completionCount.fetch_add(1, std::memory_order_release);
        }
        acceptanceMailboxScheduled[index] = false;
        ScheduleDrain();
    }

    void DrainAcceptanceMailbox(std::size_t index, const TaskContext& context) {
        constexpr auto yieldDuration = std::chrono::milliseconds(2);
        const auto started = std::chrono::steady_clock::now();
        std::size_t appliedCount = 0;
        std::uint64_t mutationDurationNs = 0;
        bool hasMore = false;
        do {
            AcceptanceDispatch item;
            {
                auto lock = LockMutex(GraphMutexPhase::AcceptanceDequeue);
                auto& mailbox = acceptanceMailboxes[index];
                if (mailbox.empty()) {
                    acceptanceMailboxScheduled[index] = false;
                    break;
                }
                item = std::move(mailbox.front());
                mailbox.pop_front();
            }
            bool succeeded = !context.StopRequested();
            std::string error;
            if (succeeded) {
                const auto mutationStarted = std::chrono::steady_clock::now();
                try { item.registration.action(item.snapshot); }
                catch (const std::exception& exception) {
                    succeeded = false;
                    error = exception.what();
                } catch (...) {
                    succeeded = false;
                    error = "acceptance action threw an unknown exception";
                }
                mutationDurationNs += static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - mutationStarted).count());
            } else error = "acceptance cancelled during graph shutdown";
            ++appliedCount;
            if (!succeeded) item.completion.result = ArtifactBuildResult::Failure(std::move(error));
            item.completion.queuedAt = std::chrono::steady_clock::now();
			completions.push(std::move(item.completion));
			completionCount.fetch_add(1, std::memory_order_release);
			{
				auto lock = LockMutex(GraphMutexPhase::AcceptanceCompletionEnqueue);
				hasMore = !acceptanceMailboxes[index].empty();
			}
            if (auto session = AcquireTrace()) {
                session->Record(AsyncStateGraphTraceEventID::AcceptanceApplied, item.snapshot.key,
                    item.snapshot.revision, item.snapshot.generation,
                    succeeded ? ArtifactReadiness::Preparing : ArtifactReadiness::Failed);
            }
        } while (hasMore && std::chrono::steady_clock::now() - started < yieldDuration);
        basic_telemetry::Record("SARP.AsyncStateGraph.AcceptanceBatchSize", appliedCount);
        basic_telemetry::Record("SARP.AsyncStateGraph.AcceptanceMutationDurationNs",
            mutationDurationNs);
        ScheduleDrain();
        {
            auto lock = LockMutex(GraphMutexPhase::AcceptanceDequeue);
            hasMore = !acceptanceMailboxes[index].empty();
            if (!hasMore) acceptanceMailboxScheduled[index] = false;
        }
        if (hasMore) ScheduleAcceptanceMailbox(index);
    }

    void EnqueueAcceptances(std::vector<AcceptanceDispatch> dispatches) {
        std::vector<std::size_t> schedule;
        {
            auto lock = LockMutex(GraphMutexPhase::AcceptanceDispatchEnqueue);
            for (auto& dispatch : dispatches) {
                const auto index = AcceptanceMailboxIndex(
                    dispatch.registration.lane, dispatch.registration.domain);
                acceptanceMailboxes[index].push_back(std::move(dispatch));
                if (!acceptanceMailboxScheduled[index]) {
                    acceptanceMailboxScheduled[index] = true;
                    schedule.push_back(index);
                }
            }
        }
        for (const auto index : schedule) ScheduleAcceptanceMailbox(index);
    }

    void ConfigureDrainPump() {
        auto weak = weak_from_this();
        drainPump.Configure(
            [weak](br::SerializedTaskPump::Task task) {
                auto self = weak.lock();
                const auto submittedAt = std::chrono::steady_clock::now();
                return self && self->scheduler.Submit(
                    self->scope, TaskLane::Streaming, TaskDomain::GraphControl,
                    "AsyncStateGraph::Drain",
                    [weak, submittedAt, task = std::move(task)](
                        const TaskContext& context) mutable {
                        if (context.StopRequested()) return;
                        if (auto self = weak.lock()) {
                            const auto wait = static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - submittedAt).count());
                            {
                                auto lock = self->LockMutex(GraphMutexPhase::DelayedDrainState);
                                self->stats.controlQueueWaitMicros += wait;
                                self->stats.maxControlQueueWaitMicros = (std::max)(
                                    self->stats.maxControlQueueWaitMicros, wait);
                            }
                            if (auto session = self->AcquireTrace()) {
                                session->Record(AsyncStateGraphTraceEventID::GraphControlStarted, {}, 0, 0,
                                    ArtifactReadiness::Missing, static_cast<std::int64_t>(wait));
                            }
                        }
                        task();
                    });
            },
            [weak] {
                if (auto self = weak.lock()) self->Drain();
            });
    }

    static bool IsOutstandingState(ArtifactReadiness state) {
		return state != ArtifactReadiness::GpuReady &&
			state != ArtifactReadiness::Published &&
			state != ArtifactReadiness::Superseded &&
			state != ArtifactReadiness::Cancelled &&
			state != ArtifactReadiness::Failed;
	}

	void SetDesired(Node& node, bool desired) {
		if (node.desired == desired) return;
		const auto kind = static_cast<std::size_t>(node.key.kind);
		if (kind < outstandingByKind.size() && IsOutstandingState(node.state)) {
			if (desired) ++outstandingByKind[kind];
			else if (outstandingByKind[kind] != 0) --outstandingByKind[kind];
		}
		node.desired = desired;
	}

    void SetState(Node& node, ArtifactReadiness state) {
        const auto previous = node.state;
        if (previous == state) return;
		const auto previousIndex = static_cast<std::size_t>(previous);
		const auto nextIndex = static_cast<std::size_t>(state);
		if (previousIndex < nodeStateCounts.size() && nodeStateCounts[previousIndex] != 0)
			--nodeStateCounts[previousIndex];
		if (nextIndex < nodeStateCounts.size()) ++nodeStateCounts[nextIndex];
		const auto kind = static_cast<std::size_t>(node.key.kind);
		if (node.desired && kind < outstandingByKind.size()) {
			const bool wasOutstanding = IsOutstandingState(previous);
			const bool isOutstanding = IsOutstandingState(state);
			if (!wasOutstanding && isOutstanding) ++outstandingByKind[kind];
			else if (wasOutstanding && !isOutstanding && outstandingByKind[kind] != 0)
				--outstandingByKind[kind];
		}
        node.state = state;
        node.stateSince = std::chrono::steady_clock::now();
        if (auto session = AcquireTrace()) {
            session->Record(AsyncStateGraphTraceEventID::StateChanged, node.key, node.desiredRevision,
                node.versionGeneration, state, 0,
                { { static_cast<unsigned>(previous), static_cast<unsigned>(state) } });
        }
    }

    std::uint64_t VersionGeneration(const ArtifactKey& address, std::uint64_t revision) const {
        const auto found = versionGenerations.find({ address, revision });
        return found == versionGenerations.end() ? 0 : found->second;
    }

    ArtifactSnapshot MakeSnapshot(const Node& node) const {
        return { node.key, node.producedRevision,
            node.versionGeneration, node.state, node.payload,
            node.gpuSubmissions, node.lease };
    }

    ArtifactLease AcquireVersionLease(const StoredVersionKey& version) const {
        if (const auto found = versionLeases.find(version); found != versionLeases.end()) {
            if (auto lease = found->second.lock()) return ArtifactLease{ std::move(lease) };
        }
        auto weak = const_cast<Impl*>(this)->weak_from_this();
        auto lease = std::shared_ptr<const void>(new std::uint8_t(0),
			[weak, version](const void* value) {
                delete static_cast<const std::uint8_t*>(value);
				if (auto graph = weak.lock()) {
					graph->reclaimQueue.push(version);
					graph->ScheduleDrain();
				}
            });
        versionLeases.insert_or_assign(version, lease);
        return ArtifactLease{ std::move(lease) };
    }

	ArtifactSnapshot SnapshotExactLocked(ArtifactVersionID version) const {
		const auto archived = versions.find({ version.address, version.revision, version.generation });
		if (archived != versions.end()) {
			auto snapshot = archived->second;
			snapshot.lease = AcquireVersionLease({ version.address, version.revision, version.generation });
			return snapshot;
		}
		const auto current = nodes.find(version.address);
		if (current == nodes.end()) return { version.address, version.revision, version.generation };
		if (current->second.producedRevision == version.revision &&
			current->second.versionGeneration == version.generation) {
			auto snapshot = MakeSnapshot(current->second);
			if (snapshot.generation == version.generation) return snapshot;
		}
		if (current->second.desiredRevision == version.revision &&
			current->second.versionGeneration == version.generation) {
			return { version.address, version.revision, version.generation,
				current->second.state, current->second.payload,
				current->second.gpuSubmissions, current->second.lease };
		}
		const auto successor = std::ranges::find_if(current->second.successors,
			[&](const RequestedVersion& candidate) {
				return candidate.revision == version.revision &&
					candidate.generation == version.generation;
			});
		if (successor != current->second.successors.end() &&
			successor->generation == version.generation) {
			return { version.address, version.revision, version.generation,
				ArtifactReadiness::Blocked, {}, {}, successor->lease };
		}
		return { version.address, version.revision, version.generation };
	}

    void StoreVersion(const Node& node) {
        if (node.producedRevision == 0 || !node.payload.Valid()) return;
        auto snapshot = MakeSnapshot(node);
        // The archive is storage, not a lifetime owner. Desired nodes,
        // requirements, returned handles, manifests and frame leases own pins.
        snapshot.lease.reset();
        const StoredVersionKey key{ node.key, node.producedRevision, node.versionGeneration };
        versions.insert_or_assign(key, std::move(snapshot));
        versionsByAddress[node.key].insert_or_assign(
            std::pair{ node.producedRevision, node.versionGeneration }, key);
		reclaimQueue.push(key);
    }

    bool RecipeReferences(const StoredVersionKey& version,
        const std::vector<ArtifactRequirement>& requirements) const {
        return std::ranges::any_of(requirements, [&](const ArtifactRequirement& requirement) {
            if (requirement.key != version.address) return false;
            if (requirement.invalidation == DependencyInvalidationPolicy::Latest) {
                // LatestAtLeast is not an exact-version pin, but it must retain
                // the ready version currently satisfying the dependency. If a
                // newer desired revision blocks on capacity after this archive
                // is reclaimed, the consumer and producer can otherwise wait
                // on each other forever.
                const auto* selected = SelectSnapshot(requirement);
                return selected && selected->revision == version.revision &&
                    selected->generation == version.generation;
            }
            return requirement.minimumRevision == version.revision &&
                (requirement.requiredGeneration == 0 ||
                 requirement.requiredGeneration == version.generation);
        });
    }

    static bool PinsExactRecipe(const ArtifactKey& consumer, bool coalescible,
        const ArtifactRequirement& requirement) {
        if (requirement.minimumRevision == 0 ||
            requirement.invalidation == DependencyInvalidationPolicy::Latest) return false;
        // Most latest-wins consumers may replace their entire recipe. Active
        // lists and references to them are content-bearing: their compacted
        // indices must remain paired with the exact source generation.
        return !coalescible || consumer.kind == ArtifactKind::ActiveDrawList ||
            requirement.key.kind == ArtifactKind::ActiveDrawList;
    }

    void AdjustExactRecipePins(const ArtifactKey& consumer, bool coalescible,
        const std::vector<ArtifactRequirement>& requirements, int delta) {
        for (const auto& requirement : requirements) {
            if (!PinsExactRecipe(consumer, coalescible, requirement)) continue;
            const StoredVersionKey key{ requirement.key, requirement.minimumRevision,
                requirement.requiredGeneration };
            if (delta > 0) {
                ++exactRecipePins[key];
            } else if (const auto found = exactRecipePins.find(key);
                found != exactRecipePins.end()) {
                if (found->second <= 1) exactRecipePins.erase(found);
                else --found->second;
            }
        }
    }

    void PinSuccessor(const Node& node, const RequestedVersion& successor) {
        AdjustExactRecipePins(node.key, successor.coalescibleIntent,
            successor.requirements, 1);
    }

    void UnpinSuccessor(const Node& node, const RequestedVersion& successor) {
        AdjustExactRecipePins(node.key, successor.coalescibleIntent,
            successor.requirements, -1);
    }

    void ClearSuccessors(Node& node) {
        for (const auto& successor : node.successors) UnpinSuccessor(node, successor);
        node.successors.clear();
    }

    bool HasExactDependent(const StoredVersionKey& version) const {
        if (exactRecipePins.contains(version)) return true;
        // Generation zero is a deliberate wildcard used by callers that pin
        // an exact revision before its immutable generation is known.
        return exactRecipePins.contains({ version.address, version.revision, 0 });
    }

	void ReclaimUnreferencedVersions(std::chrono::steady_clock::time_point deadline) {
		constexpr std::size_t maxCandidatesPerDrain = 64;
		auto retentionTrace = AcquireTrace();
		const bool traceIndividualVersions = retentionTrace &&
			retentionTrace->Config().includeRetentionEvents &&
			retentionTrace->Config().detail == AsyncStateGraphTraceDetail::FullDependencies;
		std::uint64_t reclaimedInBatch = 0;
		StoredVersionKey candidate;
		for (std::size_t examined = 0;
			examined < maxCandidatesPerDrain && reclaimQueue.try_pop(candidate); ++examined) {
			++stats.reclaimCandidates;
			auto version = versions.find(candidate);
			if (version == versions.end()) {
				if (const auto lease = versionLeases.find(candidate);
					lease != versionLeases.end() && lease->second.expired()) {
					versionLeases.erase(lease);
				}
				continue;
			}
			const auto lease = versionLeases.find(candidate);
			bool referenced = HasExactDependent(candidate) ||
				(lease != versionLeases.end() && !lease->second.expired());
			if (const auto dependents = waiters.find(candidate.address);
				!referenced && dependents != waiters.end()) {
				for (const auto& dependentKey : dependents->second) {
					const auto dependent = nodes.find(dependentKey);
					if (dependent == nodes.end()) continue;
					const auto& node = dependent->second;
					referenced = RecipeReferences(candidate, node.requirements) ||
						RecipeReferences(candidate, node.requestedRequirements) ||
						std::ranges::any_of(node.successors, [&](const RequestedVersion& successor) {
							return RecipeReferences(candidate, successor.requirements);
						});
					if (referenced) break;
				}
			}
			if (const auto current = nodes.find(candidate.address); current != nodes.end()) {
				const auto& node = current->second;
				referenced = referenced ||
					(node.desiredRevision == candidate.revision &&
					 node.versionGeneration == candidate.generation) ||
					(node.producedRevision == candidate.revision &&
					 node.versionGeneration == candidate.generation) ||
					std::ranges::any_of(node.successors, [&](const RequestedVersion& successor) {
						return successor.revision == candidate.revision &&
							successor.generation == candidate.generation;
					});
			}
			if (referenced) continue;
			const auto reclaimed = candidate;
			pendingRetirement.push_back(std::move(version->second));
			version = versions.erase(version);
            if (auto address = versionsByAddress.find(reclaimed.address);
                address != versionsByAddress.end()) {
                address->second.erase({ reclaimed.revision, reclaimed.generation });
                if (address->second.empty()) versionsByAddress.erase(address);
            }
            ++reclaimedVersions;
			++reclaimedInBatch;
			if (const auto lease = versionLeases.find(reclaimed);
				lease != versionLeases.end() && lease->second.expired()) {
				versionLeases.erase(lease);
			}
            basic_telemetry::AddCounter("SARP.AsyncStateGraph.ReclaimedVersions");
			if (traceIndividualVersions) {
				retentionTrace->Record(AsyncStateGraphTraceEventID::VersionReclaimed, reclaimed.address, reclaimed.revision,
                    reclaimed.generation);
            }
			if (std::chrono::steady_clock::now() >= deadline) break;
		}
		if (reclaimedInBatch != 0 && retentionTrace &&
			retentionTrace->Config().includeRetentionEvents && !traceIndividualVersions) {
			retentionTrace->Record(AsyncStateGraphTraceEventID::VersionsReclaimed, {}, 0, 0,
				ArtifactReadiness::Missing, static_cast<std::int64_t>(reclaimedInBatch));
		}
    }

    const ArtifactSnapshot* SelectSnapshot(const ArtifactRequirement& requirement) const {
        if (requirement.invalidation == DependencyInvalidationPolicy::ExactSnapshot ||
            requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold ||
            (requirement.invalidation == DependencyInvalidationPolicy::ReadyGate &&
             requirement.minimumRevision != 0)) {
            if (requirement.requiredGeneration != 0) {
                const auto exact = versions.find({ requirement.key, requirement.minimumRevision,
                    requirement.requiredGeneration });
                if (exact != versions.end()) return &exact->second;
            } else {
                const ArtifactSnapshot* newest = nullptr;
                if (const auto address = versionsByAddress.find(requirement.key);
                    address != versionsByAddress.end()) {
                    const auto first = address->second.lower_bound({ requirement.minimumRevision, 0 });
                    for (auto candidate = first; candidate != address->second.end() &&
                        candidate->first.first == requirement.minimumRevision; ++candidate) {
                        const auto archived = versions.find(candidate->second);
                        if (archived != versions.end() &&
                            (!newest || archived->second.generation > newest->generation)) {
                            newest = &archived->second;
                        }
                    }
                }
                if (newest) return newest;
            }
            const auto current = nodes.find(requirement.key);
            if (current != nodes.end() &&
                current->second.producedRevision == requirement.minimumRevision &&
                (requirement.requiredGeneration == 0 ||
                    current->second.versionGeneration == requirement.requiredGeneration)) {
                static thread_local ArtifactSnapshot selected;
                selected = MakeSnapshot(current->second);
                return &selected;
            }
            return nullptr;
        }
        // LatestAtLeast selects the newest *ready* immutable version, not
        // necessarily the address's in-progress desired cursor. Advancing an
        // address to a blocked successor must not make an already-satisfied
        // minimum false for consumers that can safely use that ready version.
        static thread_local ArtifactSnapshot currentSnapshot;
        const ArtifactSnapshot* selected = nullptr;
        if (const auto current = nodes.find(requirement.key); current != nodes.end() &&
            current->second.producedRevision >= requirement.minimumRevision) {
            currentSnapshot = MakeSnapshot(current->second);
            if (Satisfies(currentSnapshot.readiness, requirement.requiredReadiness)) {
                selected = &currentSnapshot;
            }
        }
        if (const auto address = versionsByAddress.find(requirement.key);
            address != versionsByAddress.end()) {
            for (auto candidate = address->second.rbegin(); candidate != address->second.rend();
                ++candidate) {
                if (candidate->first.first < requirement.minimumRevision) break;
                const auto archived = versions.find(candidate->second);
                if (archived == versions.end() || !Satisfies(
                    archived->second.readiness, requirement.requiredReadiness)) continue;
                if (!selected || archived->second.revision > selected->revision ||
                    (archived->second.revision == selected->revision &&
                        archived->second.generation > selected->generation)) {
                    selected = &archived->second;
                }
                // Reverse iteration is ordered by revision then generation, so
                // the first satisfying archived version is the newest one.
                break;
            }
        }
        return selected;
    }

    void RemoveWaiterEdges(Node& node) {
        AdjustExactRecipePins(node.key, node.coalescibleIntent, node.requirements, -1);
		// Latest requirements are deliberately not exact recipe pins, but the
		// resolved snapshots still made their archived versions appear referenced
		// during the first reclamation attempt. Once this consumer is replacing its
		// dependency set, explicitly reconsider those old immutable versions.
		for (const auto& dependency : node.resolvedDependencies) {
			if (dependency.revision != 0 && dependency.generation != 0) {
				reclaimQueue.push({ dependency.key, dependency.revision,
					dependency.generation });
			}
		}
        const auto remove = [&](const ArtifactRequirement& requirement) {
			if (requirement.minimumRevision != 0 &&
				requirement.invalidation != DependencyInvalidationPolicy::Latest) {
				if (requirement.requiredGeneration != 0) {
					reclaimQueue.push({ requirement.key, requirement.minimumRevision,
						requirement.requiredGeneration });
				} else if (const auto address = versionsByAddress.find(requirement.key);
					address != versionsByAddress.end()) {
					const auto first = address->second.lower_bound({ requirement.minimumRevision, 0 });
					for (auto candidate = first; candidate != address->second.end() &&
						candidate->first.first == requirement.minimumRevision; ++candidate)
						reclaimQueue.push(candidate->second);
				}
			}
        };
        for (const auto& requirement : node.requirements) remove(requirement);
		if (node.requestedRequirements != node.requirements) {
			for (const auto& requirement : node.requestedRequirements) remove(requirement);
		}
		for (const auto& dependency : node.installedWaiterKeys) {
			if (auto found = waiters.find(dependency); found != waiters.end()) {
				found->second.erase(node.key);
				if (found->second.empty()) waiters.erase(found);
			}
		}
		node.installedWaiterKeys.clear();
		node.completedWaiterEdgesPruned = false;
    }

	void RebuildRequestedRequirementIndex(Node& node) {
		constexpr std::size_t kIndexedRecipeThreshold = 32;
		node.requestedRequirementIndex.clear();
		if (node.requestedRequirements.size() <= kIndexedRecipeThreshold) return;
		node.requestedRequirementIndex.reserve(node.requestedRequirements.size());
		for (std::size_t index = 0; index < node.requestedRequirements.size(); ++index)
			node.requestedRequirementIndex.try_emplace(
				node.requestedRequirements[index].key, index);
	}

	void RebuildResolvedDependencyIndex(Node& node) {
		constexpr std::size_t kIndexedRecipeThreshold = 32;
		node.resolvedDependencyIndex.clear();
		if (node.resolvedDependencies.size() <= kIndexedRecipeThreshold) return;
		node.resolvedDependencyIndex.reserve(node.resolvedDependencies.size());
		for (const auto& dependency : node.resolvedDependencies) {
			node.resolvedDependencyIndex.try_emplace(dependency.key,
				std::pair{ dependency.revision, dependency.generation });
		}
	}

    void InstallWaiterEdges(Node& node) {
		RebuildRequestedRequirementIndex(node);
		AdjustExactRecipePins(node.key, node.coalescibleIntent, node.requirements, 1);
		node.installedWaiterKeys.clear();
		node.installedWaiterKeys.reserve(node.requirements.size() +
			node.requestedRequirements.size());
		std::unordered_set<ArtifactKey, ArtifactKey::Hasher> unique;
		unique.reserve(node.requirements.size() + node.requestedRequirements.size());
		const auto install = [&](const ArtifactRequirement& requirement) {
			if (!unique.insert(requirement.key).second) return;
			waiters[requirement.key].insert(node.key);
			node.installedWaiterKeys.push_back(requirement.key);
		};
		for (const auto& requirement : node.requirements) install(requirement);
		for (const auto& requirement : node.requestedRequirements) install(requirement);
		node.completedWaiterEdgesPruned = false;
    }

	// Once a consumer has produced its immutable version, exact/ready-gate edges
	// no longer need wake notifications. Their lifetime pins are tracked
	// independently by exactRecipePins and the captured dependency leases. Keep
	// only Latest edges subscribed so advancing an address can request a derived
	// successor. Without this pruning, every shared lifetime or buffer update
	// revisited all historical manifests and groups even though QueueNode would
	// immediately reject them as already complete.
	void PruneCompletedWaiterEdges(Node& node) {
		std::unordered_set<ArtifactKey, ArtifactKey::Hasher> latest;
		latest.reserve(node.requestedRequirements.size());
		for (const auto& requirement : node.requestedRequirements) {
			if (requirement.invalidation == DependencyInvalidationPolicy::Latest)
				latest.insert(requirement.key);
		}
		std::vector<ArtifactKey> retained;
		retained.reserve((std::min)(latest.size(), node.installedWaiterKeys.size()));
		for (const auto& dependency : node.installedWaiterKeys) {
			if (latest.contains(dependency)) {
				retained.push_back(dependency);
				continue;
			}
            if (auto found = waiters.find(dependency); found != waiters.end()) {
				found->second.erase(node.key);
				if (found->second.empty()) waiters.erase(found);
			}
		}
		node.installedWaiterKeys = std::move(retained);
		node.completedWaiterEdgesPruned = true;
	}

    std::vector<ArtifactKey> DetectCycle(const Node& node) {
		// Search the forward dependency closure once from all new requirements.
		// The old implementation repeated a full DFS for every requirement; the
		// interim reverse-index search instead exploded through every consumer of
		// common StaticGroup/StaticTemplate nodes. Mutation-owned visit epochs keep
		// this traversal allocation-free apart from its bounded work stack.
		if (++nextCycleVisitEpoch == 0) {
			for (auto& [_, candidate] : nodes) candidate.cycleVisitEpoch = 0;
			++nextCycleVisitEpoch;
		}
		const auto epoch = nextCycleVisitEpoch;
		std::vector<ArtifactKey> pendingSearch;
		pendingSearch.reserve(node.requirements.size());
		for (const auto& requirement : node.requirements) {
			if (requirement.policy == DependencyPolicy::Optional ||
				requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
			if (requirement.key == node.key) return { node.key, node.key };
			auto root = nodes.find(requirement.key);
			if (root == nodes.end() || root->second.cycleVisitEpoch == epoch) continue;
			root->second.cycleVisitEpoch = epoch;
			root->second.cycleVisitParent = node.key;
			pendingSearch.push_back(requirement.key);
		}

		while (!pendingSearch.empty()) {
			const auto currentKey = pendingSearch.back();
			pendingSearch.pop_back();
			auto current = nodes.find(currentKey);
			if (current == nodes.end()) continue;
			for (const auto& requirement : current->second.requirements) {
				if (requirement.policy == DependencyPolicy::Optional ||
					requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
				if (requirement.key == node.key) {
					std::vector<ArtifactKey> reversePath;
					auto cursor = currentKey;
					while (cursor != node.key) {
						reversePath.push_back(cursor);
						const auto visited = nodes.find(cursor);
						if (visited == nodes.end() || !visited->second.cycleVisitParent) break;
						cursor = *visited->second.cycleVisitParent;
					}
					std::ranges::reverse(reversePath);
					std::vector<ArtifactKey> path;
					path.reserve(reversePath.size() + 2u);
					path.push_back(node.key);
					path.insert(path.end(), reversePath.begin(), reversePath.end());
					path.push_back(node.key);
					return path;
				}
				auto dependency = nodes.find(requirement.key);
				if (dependency == nodes.end() || dependency->second.cycleVisitEpoch == epoch) continue;
				dependency->second.cycleVisitEpoch = epoch;
				dependency->second.cycleVisitParent = currentKey;
				pendingSearch.push_back(requirement.key);
			}
		}
		return {};
    }

    std::string CycleString(const std::vector<ArtifactKey>& cycle) const {
        std::string message;
        for (const auto& key : cycle) {
            if (!message.empty()) message += " -> ";
            message += KeyString(key);
        }
        return message;
    }

    void FailCycle(const std::vector<ArtifactKey>& cycle) {
        const auto error = "dependency cycle: " + CycleString(cycle);
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher> unique;
        for (const auto& key : cycle) {
            if (!unique.insert(key).second) continue;
            const auto found = nodes.find(key);
            if (found == nodes.end()) continue;
            found->second.error = error;
            found->second.terminalFailure = true;
            ++found->second.generation;
            found->second.retryAt.reset();
            SetState(found->second, ArtifactReadiness::Failed);
            ++stats.failed;
        }
        ++stats.cycles;
    }

    bool RequirementSatisfied(const ArtifactRequirement& requirement) const {
        ++const_cast<Impl*>(this)->stats.dependencyEvaluations;
        const auto* snapshot = SelectSnapshot(requirement);
        return snapshot && Satisfies(snapshot->readiness, requirement.requiredReadiness);
    }

    // Address-level ReadyGate is a one-shot synchronization edge. Once any
    // immutable version has reached the requested milestone, bind the consumer
    // to that exact version. Leaving minimumRevision at zero made the edge
    // follow the mutable address cursor: requesting a blocked successor could
    // therefore make an already-satisfied gate false again and strand an
    // unrelated consumer (most visibly indirect state behind material tables).
    void LatchReadyGates(Node& node) {
        for (auto& requirement : node.requirements) {
            if (requirement.invalidation != DependencyInvalidationPolicy::ReadyGate ||
                requirement.minimumRevision != 0) continue;

            const ArtifactSnapshot* selected = nullptr;
            if (const auto current = nodes.find(requirement.key); current != nodes.end()) {
                static thread_local ArtifactSnapshot currentSnapshot;
                currentSnapshot = MakeSnapshot(current->second);
                if (currentSnapshot.revision != 0 &&
                    Satisfies(currentSnapshot.readiness, requirement.requiredReadiness)) {
                    selected = &currentSnapshot;
                }
            }
            if (const auto address = versionsByAddress.find(requirement.key);
                address != versionsByAddress.end()) {
                for (auto candidate = address->second.rbegin(); candidate != address->second.rend();
                    ++candidate) {
                    const auto archived = versions.find(candidate->second);
                    if (archived == versions.end() || !Satisfies(
                        archived->second.readiness, requirement.requiredReadiness)) continue;
                    if (!selected || archived->second.revision > selected->revision) {
                        selected = &archived->second;
                    }
                    break;
                }
            }
            if (!selected) continue;
            requirement.minimumRevision = selected->revision;
            requirement.requiredGeneration = selected->generation;
            if (PinsExactRecipe(node.key, node.coalescibleIntent, requirement)) {
                ++exactRecipePins[{ requirement.key, requirement.minimumRevision,
                    requirement.requiredGeneration }];
            }
        }
    }

    bool DependenciesSatisfied(const Node& node) const {
        // Optional and lifetime-only edges contribute snapshots and ownership to
        // the producer context, but they can never gate admission.  Resolving
        // them here is especially expensive for aggregate artifacts such as
        // TerrainState, which can carry thousands of optional texture bindings
        // and is reconsidered whenever any one of those bindings advances.
        std::vector<std::pair<std::uint32_t, bool>> anyGroups;
        std::vector<std::pair<std::uint32_t, bool>> fallbackGroups;
        const auto recordAlternative = [](auto& groups, std::uint32_t identity,
            bool satisfied) {
            const auto found = std::ranges::find(groups, identity,
                &std::pair<std::uint32_t, bool>::first);
            if (found == groups.end()) groups.emplace_back(identity, satisfied);
            else found->second |= satisfied;
        };
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional ||
                requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
            const bool satisfied = RequirementSatisfied(requirement);
            switch (requirement.policy) {
            case DependencyPolicy::Optional: break;
            case DependencyPolicy::AnyOf:
                recordAlternative(anyGroups, requirement.alternativeGroup, satisfied);
                break;
            case DependencyPolicy::AllOf:
                if (!satisfied) return false;
                break;
            case DependencyPolicy::FallbackAllowed:
                recordAlternative(fallbackGroups, requirement.alternativeGroup, satisfied);
                break;
            }
        }
        for (const auto& [_, satisfied] : anyGroups) if (!satisfied) return false;
        for (const auto& [_, satisfied] : fallbackGroups) if (!satisfied) return false;
        return true;
    }

    void AppendBlockerChain(const ArtifactKey& key,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited,
        std::string& output) const {
        if (!visited.insert(key).second) {
            output += KeyString(key) + " (cycle)";
            return;
        }
        const auto found = nodes.find(key);
        if (found == nodes.end()) {
            output += KeyString(key) + " (missing)";
            return;
        }
        const auto& node = found->second;
        output += std::format("{} rev {}/{} state {}", KeyString(key), node.producedRevision,
            node.desiredRevision, static_cast<unsigned>(node.state));
        if (!node.error.empty()) output += " error=" + node.error;
        if (node.retryAt) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                *node.retryAt - std::chrono::steady_clock::now()).count();
            output += std::format(" retry-in={}ms", (std::max)(std::int64_t{ 0 }, remaining));
        }
        if ((node.state == ArtifactReadiness::CpuReady ||
             node.state == ArtifactReadiness::UploadSubmitted) && node.waitingGpuSubmissions) {
            output += std::format(" gpu-value={}", node.waitingGpuSubmissions->MaximumTimelineValue());
			const auto detail = node.waitingGpuSubmissions->Describe();
			if (!detail.empty()) output += " " + detail;
        }
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional || RequirementSatisfied(requirement)) continue;
            output += std::format(" <- [requires rev {} gen {} readiness {} policy {} invalidation {}; ",
                requirement.minimumRevision,
                requirement.requiredGeneration,
                static_cast<unsigned>(requirement.requiredReadiness),
                static_cast<unsigned>(requirement.policy),
                static_cast<unsigned>(requirement.invalidation));
            AppendBlockerChain(requirement.key, visited, output);
            output += "]";
        }
        visited.erase(key);
    }

    std::vector<ArtifactSnapshot> DependencySnapshots(const Node& node) const {
        std::vector<ArtifactSnapshot> result;
        result.reserve(node.requirements.size());
        std::unordered_set<std::uint64_t> selectedGroups;
        for (const auto& requirement : node.requirements) {
            const bool alternative = requirement.policy == DependencyPolicy::AnyOf ||
                requirement.policy == DependencyPolicy::FallbackAllowed;
            const auto groupIdentity = (static_cast<std::uint64_t>(requirement.policy) << 32u) |
                requirement.alternativeGroup;
            if (alternative && selectedGroups.contains(groupIdentity)) continue;
            const auto* selected = SelectSnapshot(requirement);
            if (selected && Satisfies(selected->readiness, requirement.requiredReadiness)) {
                auto snapshot = *selected;
                snapshot.lease = AcquireVersionLease(
                    StoredVersionKey{ snapshot.key, snapshot.revision, snapshot.generation });
                result.push_back(std::move(snapshot));
                if (alternative) selectedGroups.insert(groupIdentity);
            }
        }
        return result;
    }

    void QueueNode(Node& node) {
        if (!node.desired || node.terminalFailure || node.buildInFlight ||
            node.state == ArtifactReadiness::Queued ||
            (node.state == ArtifactReadiness::CpuReady && node.waitingGpuSubmissions) ||
            node.state == ArtifactReadiness::UploadSubmitted) return;
        if (node.producedRevision == node.desiredRevision &&
            (node.state == ArtifactReadiness::GpuReady ||
             node.state == ArtifactReadiness::Published)) return;
        auto phaseTrace = AcquireTrace();
        const auto latchStarted = phaseTrace
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        LatchReadyGates(node);
        const auto latchDone = phaseTrace
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const bool dependenciesSatisfied = DependenciesSatisfied(node);
        const auto dependenciesDone = phaseTrace
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto recordQueuePhase = [&](std::string_view phase,
            std::chrono::steady_clock::time_point begin,
            std::chrono::steady_clock::time_point end) {
            if (!phaseTrace) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
            if (elapsed < 2'000) return;
            phaseTrace->Record(AsyncStateGraphTraceEventID::QueueNodePhase, node.key, node.desiredRevision,
                node.versionGeneration, node.state, elapsed, { { StableTraceID(phase) } });
        };
        recordQueuePhase("latch_ready_gates", latchStarted, latchDone);
        recordQueuePhase("dependencies_satisfied", latchDone, dependenciesDone);
        if (!dependenciesSatisfied) {
            const bool newlyBlocked = node.state != ArtifactReadiness::Blocked;
            SetState(node, ArtifactReadiness::Blocked);
            if (auto session = AcquireTrace();
                newlyBlocked && session && session->Config().includeDependencyEvents) {
                for (const auto& requirement : node.requirements) {
                    if (!RequirementSatisfied(requirement)) {
                        session->Record(AsyncStateGraphTraceEventID::DependencyBlocked, node.key, node.desiredRevision,
                            node.versionGeneration, node.state, 0,
                            { { static_cast<unsigned>(requirement.invalidation),
                                static_cast<unsigned>(requirement.requiredReadiness),
                                requirement.requiredGeneration } },
                            requirement.key, requirement.minimumRevision);
                    }
                }
            }
            return;
        }
        SetState(node, ArtifactReadiness::Queued);
        node.queuedAt = std::chrono::steady_clock::now();
        pending.push_back(node.key);
    }

    bool PromoteSuccessor(Node& node) {
        if (node.successors.empty() || node.buildInFlight) return false;
        StoreVersion(node);
        // Submission is a publication/scheduling milestone, not an ownership
        // lock on the logical address.  Once a version carries its immutable
        // submission set, a successor may build without waiting for that GPU
        // work to complete.  The archived snapshot retains the resources and
        // queue waits needed by exact consumers.
        if (node.state == ArtifactReadiness::UploadSubmitted) {
            node.waitingGpuSubmissions.reset();
            if (stats.gpuWaiting) --stats.gpuWaiting;
        }
        RemoveWaiterEdges(node);
        auto successor = std::move(node.successors.front());
        UnpinSuccessor(node, successor);
        node.successors.pop_front();
        node.desiredRevision = successor.revision;
        node.versionGeneration = successor.generation;
        node.requestFingerprint = successor.fingerprint;
        node.lease = std::move(successor.lease);
        node.requirements = std::move(successor.requirements);
        node.requestedRequirements = std::move(successor.requestedRequirements);
        node.input = std::move(successor.input);
        node.checkpoint = {};
        node.payload = {};
        node.resolvedDependencies.clear();
		node.resolvedDependencyIndex.clear();
        node.gpuSubmissions.reset();
        node.waitingGpuSubmissions.reset();
        node.producedRevision = 0;
        node.published = false;
        node.terminalFailure = false;
        node.latestSuccessorNeeded = false;
        node.coalescibleIntent = successor.coalescibleIntent;
        node.superseded = std::make_shared<std::atomic_bool>(false);
        node.buildAttempted = false;
        node.error.clear();
        node.retryAt.reset();
        node.generation = successor.generation;
        InstallWaiterEdges(node);
        SetState(node, ArtifactReadiness::Missing);
        if (const auto cycle = DetectCycle(node); !cycle.empty()) FailCycle(cycle);
        else QueueNode(node);
        return true;
    }

    bool QueueLatestSuccessor(Node& node) {
        if (node.buildInFlight || node.producedRevision == 0 ||
            (node.state != ArtifactReadiness::UploadSubmitted &&
             node.state != ArtifactReadiness::GpuReady &&
             node.state != ArtifactReadiness::Published)) return false;

        // A Latest edge describes a derived address, not permission to mutate
        // its completed version. Mint an internal successor so exact handles,
        // manifests, and in-flight frame leases retain the old closure.
        const auto revision = node.desiredRevision;
        if (!node.successors.empty()) {
            node.latestSuccessorNeeded = false;
            return PromoteSuccessor(node);
        }
        const auto generation = ++nextVersionGeneration;
        std::uint64_t fingerprint = node.requestFingerprint;
        HashRequestValue(fingerprint, revision);
		// This successor is minted internally after a selected Latest edge
		// advances. Its immutable graph generation is the identity boundary; the
		// producer captures the exact selected dependency closure when it starts.
		// Re-resolving every Latest requirement here made one buffer wake scan an
		// entire frame manifest (and one texture wake scan all terrain bindings)
		// while holding the graph mutex.
		HashRequestValue(fingerprint, generation);
        if (fingerprint == 0) fingerprint = 1;
        node.successors.push_back({ revision, generation, fingerprint, node.input,
            node.requestedRequirements, node.requestedRequirements,
            AcquireVersionLease({ node.key, revision, generation }),
            node.coalescibleIntent });
        PinSuccessor(node, node.successors.back());
        node.latestSuccessorNeeded = false;
        ++stats.requests;
        return PromoteSuccessor(node);
    }

    void WakeWaiter(const ArtifactKey& key, const ArtifactKey& waiter) {
            const auto nodeFound = nodes.find(waiter);
            if (nodeFound == nodes.end()) return;
            auto& node = nodeFound->second;

            bool keyIsSelected = false;
            DependencyInvalidationPolicy invalidation = DependencyInvalidationPolicy::Latest;
			const ArtifactRequirement* selectedRequirement = nullptr;
			if (!node.requestedRequirementIndex.empty()) {
				if (const auto indexed = node.requestedRequirementIndex.find(key);
					indexed != node.requestedRequirementIndex.end())
					selectedRequirement = std::addressof(
						node.requestedRequirements[indexed->second]);
			} else {
				const auto found = std::ranges::find(node.requestedRequirements, key,
					&ArtifactRequirement::key);
				if (found != node.requestedRequirements.end())
					selectedRequirement = std::addressof(*found);
			}
			if (selectedRequirement) {
				const auto& requirement = *selectedRequirement;
                invalidation = requirement.invalidation;
                if (requirement.policy == DependencyPolicy::AnyOf ||
                    requirement.policy == DependencyPolicy::FallbackAllowed) {
                    for (const auto& candidate : node.requestedRequirements) {
                        if (candidate.policy == requirement.policy &&
                            candidate.alternativeGroup == requirement.alternativeGroup &&
                            RequirementSatisfied(candidate)) {
                            keyIsSelected = candidate.key == key;
                            break;
                        }
                    }
                } else {
                    keyIsSelected = RequirementSatisfied(requirement);
                }
            }

            const auto current = nodes.find(key);
			bool exactSelectionAlreadyUsed = false;
			if (current != nodes.end()) {
				if (!node.resolvedDependencyIndex.empty()) {
					const auto captured = node.resolvedDependencyIndex.find(key);
					exactSelectionAlreadyUsed = captured != node.resolvedDependencyIndex.end() &&
						captured->second.first == current->second.producedRevision &&
						captured->second.second == current->second.versionGeneration;
				} else {
					exactSelectionAlreadyUsed = std::ranges::any_of(node.resolvedDependencies,
						[&](const ArtifactSnapshot& dependency) {
							return dependency.key == key &&
								dependency.revision == current->second.producedRevision &&
								dependency.generation == current->second.versionGeneration;
						});
				}
			}
            const bool completedConsumer =
                node.state == ArtifactReadiness::GpuReady ||
                node.state == ArtifactReadiness::Published ||
                node.state == ArtifactReadiness::UploadSubmitted;
            if (completedConsumer && keyIsSelected && !exactSelectionAlreadyUsed &&
                invalidation == DependencyInvalidationPolicy::Latest) {
                QueueLatestSuccessor(node);
                return;
            }
            if (node.buildAttempted && keyIsSelected && !exactSelectionAlreadyUsed &&
                invalidation == DependencyInvalidationPolicy::Latest) {
                node.latestSuccessorNeeded = true;
            }
            QueueNode(node);
    }

    void WakeWaiters(const ArtifactKey& key) {
        const auto found = waiters.find(key);
        if (found == waiters.end() || found->second.empty()) return;
		PendingWaiterWake wake;
		wake.dependency = key;
		wake.consumers.reserve(found->second.size());
		wake.consumers.insert(wake.consumers.end(),
			found->second.begin(), found->second.end());
		pendingWaiterWakes.push_back(std::move(wake));
        }

    bool DependenciesStillMatch(const Completion& completion) const {
        for (const auto& dependency : completion.dependencies) {
            // A build owns the exact dependency closure captured in its context.
            // Advancing a Latest address requests a successor; it must not make
            // the already-running immutable version stale.  Only disappearance
            // or regression of the captured version invalidates its completion.
            bool foundCaptured = false;
            ArtifactReadiness capturedReadiness = ArtifactReadiness::Missing;
            const auto active = nodes.find(dependency.key);
            if (active != nodes.end() &&
                active->second.producedRevision == dependency.revision &&
                active->second.versionGeneration == dependency.generation) {
                foundCaptured = true;
                capturedReadiness = active->second.state;
            }
            if (!foundCaptured) {
                const auto archived = versions.find(StoredVersionKey{
                    dependency.key, dependency.revision, dependency.generation });
                if (archived != versions.end()) {
                    foundCaptured = true;
                    capturedReadiness = archived->second.readiness;
                }
            }
            if (!foundCaptured || !Satisfies(capturedReadiness, dependency.readiness)) return false;
        }
        return true;
    }

    void ScheduleDrain(std::chrono::steady_clock::duration delay = {}) {
        if (shuttingDown.load(std::memory_order_acquire)) return;
        if (delay == std::chrono::steady_clock::duration{}) {
            (void)drainPump.Notify();
            return;
        }
        if (delayedDrainScheduled.exchange(true, std::memory_order_acq_rel)) return;
        auto weak = weak_from_this();
        const bool submitted = scheduler.ScheduleAfter(
            scope, delay, TaskLane::Streaming, TaskDomain::GraphControl,
            "AsyncStateGraph::DelayedDrain",
            [weak](const TaskContext& context) {
                if (auto self = weak.lock()) {
                    self->delayedDrainScheduled.store(false, std::memory_order_release);
                    if (!context.StopRequested()) (void)self->drainPump.Notify();
                }
            });
        if (!submitted) delayedDrainScheduled.store(false, std::memory_order_release);
    }

	void FlushDeferredRequestTrace(RequestDeferredCleanup& cleanup);

    void SubmitBuild(const ArtifactProducerRegistration& registration, ArtifactBuildContext context,
        std::shared_ptr<const std::atomic_bool> superseded) {
        auto weak = weak_from_this();
        const auto key = context.key;
        const auto revision = context.revision;
        const auto generation = context.generation;
        const auto taskKind = StableTraceID(registration.taskName.empty()
            ? std::string_view("AsyncStateGraph::Build")
            : std::string_view(registration.taskName));
        const auto correlationID = TraceCorrelationID(key, revision, generation);
        auto dependencyStamp = context.dependencies;
        if (auto session = AcquireTrace()) {
            session->Record(AsyncStateGraphTraceEventID::BuildSubmitted, key, revision, generation,
                ArtifactReadiness::Preparing, 0, { { taskKind, correlationID } });
			if (session->Config().includeDependencyEvents) {
				for (const auto& dependency : context.dependencies) {
					session->Record(AsyncStateGraphTraceEventID::BuildDependencyResolved, key, revision, generation,
						dependency.readiness, 0,
						{ { dependency.generation } },
						dependency.key, dependency.revision);
				}
			}
        }
        const bool submitted = scheduler.Submit(scope, registration.lane, registration.domain,
            registration.taskName.empty() ? "AsyncStateGraph::Build" : registration.taskName,
            [weak, registration, context = std::move(context), key, revision, generation,
                taskKind, correlationID,
                dependencyStamp = std::move(dependencyStamp),
                superseded = std::move(superseded)](const TaskContext& cancellation) mutable {
                auto self = weak.lock();
                if (!self) return;
                const auto producerStarted = std::chrono::steady_clock::now();
                if (auto session = self->AcquireTrace()) {
                    session->Record(AsyncStateGraphTraceEventID::BuildStarted, key, revision, generation,
                        ArtifactReadiness::Preparing, 0, { { taskKind, correlationID } });
                }
                context.stopRequested = [cancellation, superseded] {
                    return cancellation.StopRequested() ||
                        (superseded && superseded->load(std::memory_order_acquire));
                };
                ArtifactBuildResult result;
                try {
                    result = cancellation.StopRequested()
                        ? ArtifactBuildResult::Cancelled()
                        : registration.producer(context);
                } catch (const std::exception& exception) {
                    result = ArtifactBuildResult::Failure(exception.what());
                } catch (...) {
                    result = ArtifactBuildResult::Failure("unknown producer exception");
                }
                if (auto session = self->AcquireTrace()) {
                    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - producerStarted).count();
                    session->Record(AsyncStateGraphTraceEventID::BuildCompleted, key, revision, generation,
                        ArtifactReadiness::Preparing, duration,
                        { { static_cast<unsigned>(result.outcome), taskKind, correlationID } });
                }
				self->completions.push({ key, revision, generation,
					std::move(dependencyStamp), std::move(result) });
				self->completionCount.fetch_add(1, std::memory_order_release);
                self->ScheduleDrain();
            }, TaskTraceMetadata{
                .taskKind = taskKind,
                .correlationID = correlationID,
                .admissionKey = registration.scheduling.admissionKey,
                .workClass = static_cast<std::uint8_t>(registration.scheduling.initialClass),
                .schedulingReason = 0,
                .admissionGroup = registration.scheduling.admissionGroup });
        if (!submitted) {
            if (auto session = AcquireTrace()) {
                session->Record(AsyncStateGraphTraceEventID::BuildRejected, key, revision, generation,
                    ArtifactReadiness::Failed);
            }
			completions.push({ key, revision, generation, std::move(dependencyStamp),
				ArtifactBuildResult::Failure("scheduler rejected producer") });
			completionCount.fetch_add(1, std::memory_order_release);
            ScheduleDrain();
        }
    }

    bool PrepareAcceptance(Completion& completion,
        std::vector<AcceptanceDispatch>& dispatches) {
        auto& result = completion.result;
        if (result.outcome != ArtifactBuildResult::Outcome::Ready) return false;
        if (!result.acceptance.action && result.onAccepted) {
            result.acceptance.lane = TaskLane::Streaming;
            result.acceptance.domain = TaskDomain::RendererState;
            result.acceptance.action = std::move(result.onAccepted);
        }
        if (!result.acceptance.action) return false;
        const auto found = nodes.find(completion.key);
        if (found == nodes.end() || found->second.generation != completion.generation ||
            !DependenciesStillMatch(completion)) return false;
        const auto producer = producers.find(found->second.key.kind);
        if (producer != producers.end() &&
            producer->second.outputType != std::type_index(typeid(void)) &&
            result.payload.Type() != producer->second.outputType) return false;

        ArtifactSnapshot snapshot{ found->second.key, completion.revision,
            found->second.versionGeneration, ArtifactReadiness::Preparing,
            result.payload, result.gpuSubmissions, found->second.lease };
        auto registration = std::move(result.acceptance);
        result.onAccepted = {};
        result.acceptance = {};
        if (auto session = AcquireTrace()) {
            session->Record(AsyncStateGraphTraceEventID::AcceptanceQueued, completion.key, completion.revision,
                completion.generation, ArtifactReadiness::Preparing);
        }
        dispatches.push_back({ std::move(completion), std::move(registration),
            std::move(snapshot) });
        return true;
    }

    void ApplyCompletion(Completion& completion, std::vector<ArtifactSnapshot>& ready,
        std::vector<std::pair<std::function<void(const ArtifactSnapshot&)>, ArtifactSnapshot>>& accepted,
		std::vector<PendingGpuSubscription>& pendingGpuSubscriptions,
		std::vector<ArtifactPayload>& deferredPayloadRelease,
		std::vector<std::vector<ArtifactSnapshot>>& deferredSnapshotRelease,
		std::vector<std::uint64_t>& buildLatencySamples,
		std::uint64_t& staleCompletionTelemetry) {
		auto completionTrace = AcquireTrace();
		auto completionPhaseStarted = completionTrace
			? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
		const auto recordCompletionPhase = [&](std::string_view phase) {
			if (!completionTrace) return;
			const auto now = std::chrono::steady_clock::now();
			const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
				now - completionPhaseStarted).count();
			completionPhaseStarted = now;
			if (elapsed < 2'000) return;
			completionTrace->Record(AsyncStateGraphTraceEventID::QueueNodePhase,
				completion.key, completion.revision, completion.generation,
				ArtifactReadiness::Preparing, elapsed, { { StableTraceID(phase) } });
		};
        const auto applyDelay = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - completion.queuedAt).count());
        stats.completionApplyMicros += applyDelay;
        stats.maxCompletionApplyMicros = (std::max)(stats.maxCompletionApplyMicros, applyDelay);
        if (auto session = AcquireTrace()) {
            session->Record(AsyncStateGraphTraceEventID::CompletionApplied, completion.key, completion.revision,
                completion.generation, ArtifactReadiness::Preparing,
                static_cast<std::int64_t>(applyDelay));
        }
        const auto found = nodes.find(completion.key);
        if (found == nodes.end()) return;
        auto& node = found->second;
        if (node.generation != completion.generation || !DependenciesStillMatch(completion)) {
			recordCompletionPhase("completion_stale_validate");
            ++stats.staleCompletions;
			++staleCompletionTelemetry;
            node.buildInFlight = false;
            if (auto session = AcquireTrace()) {
                session->Record(AsyncStateGraphTraceEventID::CompletionStale, completion.key, completion.revision,
                    completion.generation, node.state, 0, { { node.generation } });
            }
            QueueNode(node);
            return;
        }
		recordCompletionPhase("completion_validate");
        node.buildInFlight = false;
        ++stats.buildsCompleted;
        const auto completedKindIndex = static_cast<std::size_t>(completion.key.kind);
        if (completedKindIndex < stats.buildsCompletedByKind.size())
            ++stats.buildsCompletedByKind[completedKindIndex];
        if (node.buildStartedAt != std::chrono::steady_clock::time_point{}) {
            const auto buildDuration = std::chrono::steady_clock::now() - node.buildStartedAt;
            stats.buildMicros += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(buildDuration).count());
			buildLatencySamples.push_back(static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(buildDuration).count()));
        }
        auto& result = completion.result;
        if (result.outcome == ArtifactBuildResult::Outcome::Ready) {
            const auto producer = producers.find(node.key.kind);
            if (producer != producers.end() &&
                producer->second.outputType != std::type_index(typeid(void)) &&
                result.payload.Type() != producer->second.outputType) {
                result = ArtifactBuildResult::Failure("artifact output type mismatch");
            }
        }
        switch (result.outcome) {
        case ArtifactBuildResult::Outcome::Ready: {
            node.terminalFailure = false;
			if (node.payload.Valid()) deferredPayloadRelease.push_back(std::move(node.payload));
            node.payload = std::move(result.payload);
			for (const auto& dependency : node.resolvedDependencies) {
				if (dependency.revision != 0 && dependency.generation != 0) {
					reclaimQueue.push({ dependency.key, dependency.revision,
						dependency.generation });
				}
			}
			if (!node.resolvedDependencies.empty())
				deferredSnapshotRelease.push_back(std::move(node.resolvedDependencies));
			node.resolvedDependencyIndex.clear();
			const bool retainsLatestDependencies = std::ranges::any_of(
				node.requestedRequirements, [](const ArtifactRequirement& requirement) {
					return requirement.invalidation == DependencyInvalidationPolicy::Latest;
				});
			if (retainsLatestDependencies) {
				node.resolvedDependencies = std::move(completion.dependencies);
				std::erase_if(node.resolvedDependencies,
					[&](const ArtifactSnapshot& dependency) {
						return !std::ranges::any_of(node.requestedRequirements,
							[&](const ArtifactRequirement& requirement) {
								return requirement.key == dependency.key &&
									requirement.invalidation ==
										DependencyInvalidationPolicy::Latest;
							});
					});
				RebuildResolvedDependencyIndex(node);
			} else if (!completion.dependencies.empty()) {
				deferredSnapshotRelease.push_back(std::move(completion.dependencies));
			}
			recordCompletionPhase("completion_payload");
			// Build input is a recipe, not published lifetime ownership. Keeping it
			// on every completed dependency-free node retained prior GPU backings;
			// FrameManifest inputs additionally retained an entire previous renderer
			// state. Only Latest recipes need the same input for automatic rebuilds.
			if (!std::ranges::any_of(node.requestedRequirements,
				[](const ArtifactRequirement& requirement) {
					return requirement.invalidation ==
						DependencyInvalidationPolicy::Latest;
				})) {
				if (node.input.Valid()) deferredPayloadRelease.push_back(std::move(node.input));
			}
			if (node.checkpoint.Valid()) deferredPayloadRelease.push_back(std::move(node.checkpoint));
			recordCompletionPhase("completion_state");
            node.retryAt.reset();
            node.suspension.reset();
            node.producedRevision = completion.revision;
            node.published = false;
            node.gpuSubmissions = std::move(result.gpuSubmissions);
			PruneCompletedWaiterEdges(node);
            if (result.onAccepted) accepted.emplace_back(
                std::move(result.onAccepted), MakeSnapshot(node));
            if (node.gpuSubmissions && !completion.gpuSubmitted) {
                node.waitingGpuSubmissions = node.gpuSubmissions;
                SetState(node, ArtifactReadiness::CpuReady);
                ++stats.gpuWaiting;
				pendingGpuSubscriptions.push_back({ node.key, node.waitingGpuSubmissions });
                if (!node.waitingGpuSubmissions->completionNotificationsAreAuthoritative)
                    gpuRecovery.push_back(node.key);
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
				recordCompletionPhase("completion_store");
                WakeWaiters(node.key);
				recordCompletionPhase("completion_wake");
            } else if (node.gpuSubmissions && !completion.gpuComplete) {
                node.waitingGpuSubmissions = node.gpuSubmissions;
                SetState(node, ArtifactReadiness::UploadSubmitted);
                node.uploadSubmittedAt = std::chrono::steady_clock::now();
                ++stats.gpuWaiting;
				pendingGpuSubscriptions.push_back({ node.key, node.waitingGpuSubmissions });
                if (!node.waitingGpuSubmissions->completionNotificationsAreAuthoritative)
                    gpuRecovery.push_back(node.key);
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
				recordCompletionPhase("completion_store");
                WakeWaiters(node.key);
				recordCompletionPhase("completion_wake");
                if (node.latestSuccessorNeeded) QueueLatestSuccessor(node);
                else PromoteSuccessor(node);
				recordCompletionPhase("completion_promote");
            } else {
                node.waitingGpuSubmissions.reset();
                SetState(node, node.published ? ArtifactReadiness::Published
                                             : ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
				recordCompletionPhase("completion_store");
                WakeWaiters(node.key);
				recordCompletionPhase("completion_wake");
                if (node.latestSuccessorNeeded) QueueLatestSuccessor(node);
                else PromoteSuccessor(node);
				recordCompletionPhase("completion_promote");
            }
            break;
        }
        case ArtifactBuildResult::Outcome::NeedsDependencies: {
            if (completion.revision != node.desiredRevision) {
                QueueNode(node);
                break;
            }
            node.terminalFailure = false;
            RemoveWaiterEdges(node);
            node.requirements = std::move(result.requirements);
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt.reset();
            InstallWaiterEdges(node);
            if (const auto cycle = DetectCycle(node); !cycle.empty()) {
                FailCycle(cycle);
            } else {
                QueueNode(node);
            }
            break;
        }
        case ArtifactBuildResult::Outcome::RetryAfter:
            if (completion.revision != node.desiredRevision) {
                QueueNode(node);
                break;
            }
            node.terminalFailure = false;
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt = std::chrono::steady_clock::now() + result.retryDelay;
            retries.push({ *node.retryAt, node.key, node.generation });
            SetState(node, ArtifactReadiness::Blocked);
            ++stats.retries;
            break;
        case ArtifactBuildResult::Outcome::Suspended: {
            if (completion.revision != node.desiredRevision || !result.suspension) {
                QueueNode(node);
                break;
            }
            node.terminalFailure = false;
            node.checkpoint = std::move(result.checkpoint);
            node.retryAt.reset();
            node.suspension = std::move(result.suspension);
            const auto& suspension = *node.suspension;
            SetState(node, ArtifactReadiness::Blocked);
            if (auto session = AcquireTrace()) {
                session->Record(AsyncStateGraphTraceEventID::SuspensionRegistered, node.key, node.desiredRevision,
                    node.generation, node.state, 0,
                    { { static_cast<unsigned>(suspension.kind), suspension.identity,
                        static_cast<unsigned>(suspension.milestone), StableTraceID(suspension.reason) } });
            }
            if (suspension.kind == ArtifactSuspensionKind::ExactDependency) {
                RemoveWaiterEdges(node);
                node.requirements.push_back(Exact(
                    suspension.dependency, suspension.milestone));
                InstallWaiterEdges(node);
                node.suspension.reset();
                QueueNode(node);
            } else if (suspension.identity == 0) {
                node.error = "suspension identity must be non-zero";
                node.terminalFailure = true;
                SetState(node, ArtifactReadiness::Failed);
                ++stats.failed;
                ready.push_back(MakeSnapshot(node));
            } else if (satisfiedSuspensions.erase(suspension.identity) != 0) {
                node.suspension.reset();
                QueueNode(node);
            } else {
                suspendedByIdentity[suspension.identity] = {
                    node.key, node.desiredRevision, node.generation };
            }
            break;
        }
        case ArtifactBuildResult::Outcome::Cancelled:
            SetState(node, ArtifactReadiness::Cancelled);
            ++stats.cancelled;
            ready.push_back({ node.key, completion.revision, completion.generation,
                node.state, {}, {}, node.lease });
            PromoteSuccessor(node);
            break;
        case ArtifactBuildResult::Outcome::Failed:
            node.error = std::move(result.error);
            node.terminalFailure = true;
            SetState(node, ArtifactReadiness::Failed);
            ++stats.failed;
            spdlog::error("AsyncStateGraph: artifact {} revision={} generation={} failed: {}",
                KeyString(node.key), node.desiredRevision, node.generation, node.error);
            ready.push_back({ node.key, completion.revision, completion.generation,
                node.state, {}, {}, node.lease });
            PromoteSuccessor(node);
            break;
        }
    }

    void Drain() {
        struct PendingGpuSignal {
            ArtifactKey key;
            std::uint64_t generation = 0;
            std::shared_ptr<const GpuSubmissionSet> token;
			std::chrono::steady_clock::time_point queuedAt{};
        };
        constexpr auto maxDuration = std::chrono::milliseconds(2);
        const auto started = std::chrono::steady_clock::now();
        auto drainTrace = AcquireTrace();
        if (drainTrace) drainTrace->Record(AsyncStateGraphTraceEventID::DrainStarted);
        std::size_t transitions = 0;
        std::vector<PendingGpuSignal> signalledGpu;
        std::vector<std::pair<std::function<void(const ArtifactSnapshot&)>, ArtifactSnapshot>> accepted;
        std::vector<AcceptanceDispatch> acceptanceDispatches;
		std::vector<GpuSignal> notifications;
		notifications.reserve(256);
		GpuSignal notification;
		while (notifications.size() < 32 && gpuSignals.try_pop(notification)) {
			notifications.push_back(std::move(notification));
			if (std::chrono::steady_clock::now() - started >= maxDuration) break;
		}
        {
            auto lock = LockMutex(GraphMutexPhase::DrainGpuCollect);
			auto gpuCollectPhaseStarted = drainTrace
				? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			const auto recordGpuCollectPhase = [&](std::string_view phase) {
				if (!drainTrace) return;
				const auto now = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					now - gpuCollectPhaseStarted).count();
				gpuCollectPhaseStarted = now;
				if (elapsed < 2'000) return;
				drainTrace->Record(AsyncStateGraphTraceEventID::DrainGpuCollectPhase, {}, 0, 0,
					ArtifactReadiness::Missing, elapsed, { { StableTraceID(phase) } });
			};
			std::unordered_set<ArtifactKey, ArtifactKey::Hasher> selectedGpuKeys;
			selectedGpuKeys.reserve(notifications.size() + 8);
			signalledGpu.reserve(notifications.size() + 8);
			for (const auto& notification : notifications) {
				++transitions;
				const auto found = nodes.find(notification.key);
                if (found != nodes.end() &&
                    (found->second.state == ArtifactReadiness::CpuReady ||
                     found->second.state == ArtifactReadiness::UploadSubmitted) &&
                    found->second.waitingGpuSubmissions) {
					selectedGpuKeys.insert(notification.key);
					signalledGpu.push_back({ notification.key, found->second.generation,
						found->second.waitingGpuSubmissions, notification.queuedAt });
                }
            }
			recordGpuCollectPhase("notification_queue");
            // Submission notifications are wakeups, not ownership-transfer events:
            // a ticket commonly notifies while its timeline is still incomplete.
            // Keep every submitted node in this rotating level-triggered set until
            // Complete() observes the timeline. A finite retry budget strands nodes
            // whenever GPU latency exceeds that budget and recreates a missed-callback
            // correctness gate in every consumer.
            const auto recoveryChecks = gpuRecovery.size();
			for (std::size_t index = 0; index < recoveryChecks && index < 32; ++index) {
                const auto recoveryKey = gpuRecovery.front();
                gpuRecovery.pop_front();
                const auto found = nodes.find(recoveryKey);
                if (found == nodes.end() ||
                    (found->second.state != ArtifactReadiness::CpuReady &&
                     found->second.state != ArtifactReadiness::UploadSubmitted) ||
					!found->second.waitingGpuSubmissions) {
					if (std::chrono::steady_clock::now() - started >= maxDuration) break;
					continue;
				}
                if (selectedGpuKeys.insert(recoveryKey).second) {
					signalledGpu.push_back({ recoveryKey, found->second.generation,
						found->second.waitingGpuSubmissions, std::chrono::steady_clock::now() });
				}
                gpuRecovery.push_back(recoveryKey);
				// Stale recovery entries are work too. Conditioning the deadline on
				// finding a live signal allowed a large stale deque to be scanned in
				// full while holding the graph mutex, producing multi-second global
				// admission stalls during streaming flight paths.
            }
			recordGpuCollectPhase("recovery_queue");
        }
        // Complete() may synchronously notify ticket subscribers. Never call
        // it while holding the graph mutex; subscribers are allowed to queue
        // another completion signal immediately.
        struct EvaluatedGpuSignal {
            PendingGpuSignal signal;
            bool submitted = false;
            bool complete = false;
			bool failed = false;
			std::string error;
        };
        std::vector<EvaluatedGpuSignal> evaluatedGpu;
        evaluatedGpu.reserve(signalledGpu.size());
        for (auto& signal : signalledGpu) {
            if (!signal.token) continue;
			const bool failed = signal.token->Failed();
            const bool submitted = signal.token->Submitted();
            const bool complete = submitted && signal.token->Complete();
			const auto failure = failed ? signal.token->Failure() : std::string{};
			evaluatedGpu.push_back({ std::move(signal), submitted, complete, failed,
				failure });
        }
        struct PendingBuild {
            ArtifactProducerRegistration registration;
            ArtifactBuildContext context;
            std::shared_ptr<const std::atomic_bool> superseded;
        };
        std::vector<PendingBuild> builds;
        std::vector<ArtifactSnapshot> ready;
        std::vector<std::function<void(const ArtifactSnapshot&)>> callbacks;
		std::vector<std::pair<ExactWaiter, ArtifactSnapshot>> exactDispatches;
		std::vector<ArtifactSnapshot> retiredVersions;
		std::vector<PendingGpuSubscription> pendingGpuSubscriptions;
		std::vector<ArtifactPayload> deferredPayloadRelease;
		std::vector<std::vector<ArtifactSnapshot>> deferredSnapshotRelease;
		std::vector<std::vector<ArtifactKey>> deferredWaiterWakeRelease;
		std::vector<std::uint64_t> buildLatencySamples;
		std::vector<std::uint64_t> gpuLatencySamples;
		std::vector<std::uint64_t> queueLatencySamples;
		buildLatencySamples.reserve(64);
		gpuLatencySamples.reserve(64);
		queueLatencySamples.reserve(64);
		std::uint64_t staleCompletionTelemetry = 0;
        std::optional<std::chrono::steady_clock::duration> retryDelay;
        bool hasImmediateWork = false;
        std::optional<AsyncStateGraphTracePayload> tracePopulation;
        struct DiagnosticSnapshot {
            std::uint64_t nodes = 0;
            std::uint64_t versions = 0;
            std::uint64_t reclaimed = 0;
            std::uint64_t exactWaiters = 0;
            std::uint64_t pending = 0;
            std::uint64_t gpuWaiting = 0;
        };
        std::optional<DiagnosticSnapshot> diagnosticSnapshot;
		std::vector<StoredVersionKey> publishedReady;
		publishedReady.reserve(32);
		StoredVersionKey publishedVersion;
		while (publishedReady.size() < 32 && publishedSignals.try_pop(publishedVersion)) {
			publishedReady.push_back(std::move(publishedVersion));
			if (std::chrono::steady_clock::now() - started >= maxDuration) break;
		}
		std::vector<Completion> readyCompletions;
		readyCompletions.reserve(16);
		Completion readyCompletion;
		while (readyCompletions.size() < 16 && completions.try_pop(readyCompletion)) {
			completionCount.fetch_sub(1, std::memory_order_acq_rel);
			if (readyCompletion.result.outcome == ArtifactBuildResult::Outcome::Ready &&
				readyCompletion.result.gpuSubmissions) {
				const auto& token = readyCompletion.result.gpuSubmissions;
				if (token->Failed()) {
					readyCompletion.result = ArtifactBuildResult::Failure(token->Failure());
				} else {
					readyCompletion.gpuSubmitted = token->Submitted();
					readyCompletion.gpuComplete = readyCompletion.gpuSubmitted && token->Complete();
					readyCompletion.gpuStateEvaluated = true;
				}
			}
			readyCompletions.push_back(std::move(readyCompletion));
			readyCompletion = {};
			if (std::chrono::steady_clock::now() - started >= maxDuration) break;
		}
        {
            auto lock = LockMutex(GraphMutexPhase::DrainApply);
			auto applyPhaseStarted = drainTrace
				? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			const auto recordApplyPhase = [&](std::string_view phase) {
				if (!drainTrace) return;
				const auto phaseDone = std::chrono::steady_clock::now();
				const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
					phaseDone - applyPhaseStarted).count();
				applyPhaseStarted = phaseDone;
				if (elapsed < 2'000) return;
				drainTrace->Record(AsyncStateGraphTraceEventID::DrainGpuCollectPhase, {}, 0, 0,
					ArtifactReadiness::Missing, elapsed, { { StableTraceID(phase) } });
			};
            const auto now = std::chrono::steady_clock::now();
			for (const auto& publishedVersion : publishedReady) {
				if (const auto archived = versions.find(publishedVersion); archived != versions.end()) {
					ready.push_back(archived->second);
				}
			}
            for (const auto& evaluation : evaluatedGpu) {
                const auto& signal = evaluation.signal;
				if (signal.queuedAt != std::chrono::steady_clock::time_point{}) {
					const auto age = static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::microseconds>(
							now - signal.queuedAt).count());
					stats.gpuApplyMicros += age;
					stats.maxGpuApplyMicros = (std::max)(stats.maxGpuApplyMicros, age);
					if (auto session = AcquireTrace()) {
						session->Record(AsyncStateGraphTraceEventID::GpuNotificationApplied, signal.key, 0,
							signal.generation, ArtifactReadiness::Missing,
							static_cast<std::int64_t>(age));
					}
				}
                const auto found = nodes.find(signal.key);
                if (found == nodes.end()) continue;
                auto& node = found->second;
                if (node.generation != signal.generation ||
                    node.waitingGpuSubmissions != signal.token) continue;
				if (evaluation.failed) {
					node.error = evaluation.error.empty()
						? "GPU submission failed" : evaluation.error;
					node.terminalFailure = true;
					node.waitingGpuSubmissions.reset();
					SetState(node, ArtifactReadiness::Failed);
					StoreVersion(node);
					ready.push_back(MakeSnapshot(node));
					WakeWaiters(node.key);
					PromoteSuccessor(node);
					++stats.failed;
					if (stats.gpuWaiting) --stats.gpuWaiting;
					continue;
				}
                if (node.state == ArtifactReadiness::CpuReady && evaluation.submitted) {
                    node.uploadSubmittedAt = now;
                    SetState(node, ArtifactReadiness::UploadSubmitted);
                    StoreVersion(node);
                    ready.push_back(MakeSnapshot(node));
                    WakeWaiters(node.key);
                    if (!evaluation.complete) {
                        if (node.latestSuccessorNeeded) QueueLatestSuccessor(node);
                        else PromoteSuccessor(node);
                        continue;
                    }
                }
                if (node.state != ArtifactReadiness::UploadSubmitted || !evaluation.complete) continue;
                if (node.uploadSubmittedAt != std::chrono::steady_clock::time_point{}) {
                    const auto gpuWait = now - node.uploadSubmittedAt;
                    stats.gpuWaitMicros += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(gpuWait).count());
					gpuLatencySamples.push_back(static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(gpuWait).count()));
                }
                node.waitingGpuSubmissions.reset();
                SetState(node, node.published ? ArtifactReadiness::Published
                                             : ArtifactReadiness::GpuReady);
                StoreVersion(node);
                ready.push_back(MakeSnapshot(node));
                WakeWaiters(node.key);
                PromoteSuccessor(node);
                if (stats.gpuWaiting) --stats.gpuWaiting;
            }
			recordApplyPhase("apply_gpu");
            while (!retries.empty()) {
                const auto retry = retries.top();
                const auto found = nodes.find(retry.key);
                const bool stale = found == nodes.end() ||
                    found->second.generation != retry.generation ||
                    !found->second.retryAt || *found->second.retryAt != retry.deadline;
                if (stale) {
                    retries.pop();
					if (std::chrono::steady_clock::now() - started >= maxDuration) break;
                    continue;
                }
                if (retry.deadline > now) {
                    retryDelay = retry.deadline - now;
                    break;
                }
                retries.pop();
                found->second.retryAt.reset();
                QueueNode(found->second);
				if (std::chrono::steady_clock::now() - started >= maxDuration) break;
            }
			recordApplyPhase("apply_retries");
			for (auto& completion : readyCompletions) {
				++transitions;
                const auto completedKey = completion.key;
				const auto completionStarted = drainTrace
					? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
				const bool preparedAcceptance = PrepareAcceptance(completion, acceptanceDispatches);
				if (drainTrace) {
					const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - completionStarted).count();
					if (elapsed >= 2'000) drainTrace->Record(
						AsyncStateGraphTraceEventID::QueueNodePhase, completedKey,
						completion.revision, completion.generation, ArtifactReadiness::Preparing,
						elapsed, { { StableTraceID("prepare_acceptance") } });
				}
				if (!preparedAcceptance) {
					ApplyCompletion(completion, ready, accepted, pendingGpuSubscriptions,
						deferredPayloadRelease, deferredSnapshotRelease,
						buildLatencySamples, staleCompletionTelemetry);
				}
				if (drainTrace) {
					const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - completionStarted).count();
					if (elapsed >= 2'000) drainTrace->Record(
						AsyncStateGraphTraceEventID::QueueNodePhase, completedKey,
						completion.revision, completion.generation, ArtifactReadiness::Preparing,
						elapsed, { { StableTraceID("apply_completion_total") } });
				}
                const auto completed = nodes.find(completedKey);
                if (completed != nodes.end() && !completed->second.desired &&
                    !completed->second.buildInFlight) {
                    const auto dependents = waiters.find(completedKey);
                    if (dependents == waiters.end() || dependents->second.empty()) {
						const auto stateIndex = static_cast<std::size_t>(completed->second.state);
						if (stateIndex < nodeStateCounts.size() && nodeStateCounts[stateIndex] != 0)
							--nodeStateCounts[stateIndex];
						nodes.erase(completed);
                    }
                }
            }
			recordApplyPhase("apply_completions");
			// Dependency fan-out is durable graph work, not part of the mutation
			// that made the dependency ready. Process it cooperatively so one
			// aggregate buffer cannot hold the graph mutex while waking hundreds
			// of manifests or shards.
			std::size_t waiterWakeSteps = 0;
			std::unordered_set<ArtifactKey, ArtifactKey::Hasher> evaluatedWaiters;
			evaluatedWaiters.reserve(16);
			while (!pendingWaiterWakes.empty() && waiterWakeSteps < 16 &&
				std::chrono::steady_clock::now() - started < maxDuration) {
				auto& wake = pendingWaiterWakes.front();
				while (wake.next < wake.consumers.size() && waiterWakeSteps < 16 &&
					std::chrono::steady_clock::now() - started < maxDuration) {
					const auto consumer = wake.consumers[wake.next++];
					++waiterWakeSteps;
					// A dependency transition is already visible in the graph state. If
					// several dependencies of one large recipe become ready in this
					// mutation slice, evaluating that consumer once observes all of them.
					// Re-running QueueNode for every edge made scene/manifests repeatedly
					// scan the same closure while holding the graph mutex.
					if (!evaluatedWaiters.insert(consumer).second) continue;
					const auto wakeStarted = drainTrace
						? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
					WakeWaiter(wake.dependency, consumer);
					if (drainTrace) {
						const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
							std::chrono::steady_clock::now() - wakeStarted).count();
						if (elapsed >= 2'000) drainTrace->Record(
							AsyncStateGraphTraceEventID::QueueNodePhase, consumer, 0, 0,
							ArtifactReadiness::Missing, elapsed,
							{ { StableTraceID("wake_waiter") } }, wake.dependency);
					}
				}
				if (wake.next != wake.consumers.size()) break;
				deferredWaiterWakeRelease.push_back(std::move(wake.consumers));
				pendingWaiterWakes.pop_front();
			}
			recordApplyPhase("apply_waiter_wakes");
			while (!pending.empty()) {
				++transitions;
                const auto key = pending.front();
                pending.pop_front();
                auto found = nodes.find(key);
                if (found == nodes.end()) continue;
                auto& node = found->second;
                if (node.state != ArtifactReadiness::Queued || node.buildInFlight) continue;
                // A newer request may replace a queued node's dependency set before
                // this queue entry is consumed. Revalidate the current closure here;
                // QueueNode's earlier check only applies to the request that queued it.
                LatchReadyGates(node);
                if (!DependenciesSatisfied(node)) {
                    SetState(node, ArtifactReadiness::Blocked);
                    continue;
                }
                const auto producer = producers.find(node.key.kind);
                if (producer == producers.end() || !producer->second.producer) {
                    node.error = "no producer registered";
                    SetState(node, ArtifactReadiness::Failed);
                    ++stats.failed;
                    ready.push_back(MakeSnapshot(node));
                    continue;
                }
                node.buildInFlight = true;
                node.buildAttempted = true;
                node.buildStartedAt = std::chrono::steady_clock::now();
                if (node.queuedAt != std::chrono::steady_clock::time_point{}) {
                    const auto queueWait = node.buildStartedAt - node.queuedAt;
                    stats.queueWaitMicros += static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(queueWait).count());
                    const auto queueKindIndex = static_cast<std::size_t>(node.key.kind);
                    if (queueKindIndex < stats.queueWaitMicrosByKind.size()) {
                        stats.queueWaitMicrosByKind[queueKindIndex] += static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::microseconds>(queueWait).count());
                    }
					queueLatencySamples.push_back(static_cast<std::uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(queueWait).count()));
                }
                SetState(node, ArtifactReadiness::Preparing);
                ++stats.buildsStarted;
                const auto buildKindIndex = static_cast<std::size_t>(node.key.kind);
                if (buildKindIndex < stats.buildsStartedByKind.size())
                    ++stats.buildsStartedByKind[buildKindIndex];
				const auto snapshotsStarted = drainTrace
					? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
				auto dependencySnapshots = DependencySnapshots(node);
				if (drainTrace) {
					const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - snapshotsStarted).count();
					if (elapsed >= 2'000) drainTrace->Record(
						AsyncStateGraphTraceEventID::QueueNodePhase, node.key,
						node.desiredRevision, node.generation, node.state, elapsed,
						{ { StableTraceID("dependency_snapshots") } });
				}
				ArtifactBuildContext context{ node.key, node.desiredRevision, node.generation,
					std::move(dependencySnapshots), node.input, node.checkpoint, {} };
                builds.emplace_back(producer->second, std::move(context), node.superseded);
				if (std::chrono::steady_clock::now() - started >= maxDuration) break;
            }
			recordApplyPhase("apply_pending_builds");
			for (const auto& snapshot : ready) {
				const StoredVersionKey versionKey{
					snapshot.key, snapshot.revision, snapshot.generation };
				const auto found = exactWaiters.find(versionKey);
				if (found == exactWaiters.end()) continue;
				auto& registered = found->second;
				for (std::size_t index = 0; index < registered.size();) {
					auto& waiter = registered[index];
					const bool terminal = snapshot.readiness == ArtifactReadiness::Failed ||
						snapshot.readiness == ArtifactReadiness::Cancelled ||
						snapshot.readiness == ArtifactReadiness::Superseded;
					if (snapshot.generation != waiter.version.generation ||
						(!terminal && !ArtifactReachedMilestone(snapshot.readiness, waiter.milestone))) {
						++index;
						continue;
					}
					exactDispatches.emplace_back(std::move(waiter), snapshot);
					if (exactWaiterCount != 0) --exactWaiterCount;
					registered[index] = std::move(registered.back());
					registered.pop_back();
				}
				if (registered.empty()) exactWaiters.erase(found);
			}
			recordApplyPhase("apply_exact_waiters");
            callbacks.reserve(readyCallbacks.size());
            for (const auto& [_, callback] : readyCallbacks) callbacks.push_back(callback);
			hasImmediateWork = !pending.empty() || !pendingWaiterWakes.empty() ||
				completionCount.load(std::memory_order_acquire) != 0 ||
				!gpuSignals.empty() || !publishedSignals.empty() || !pendingRetirement.empty();
            if (!gpuRecovery.empty() && !pauseGpuRecovery.load(std::memory_order_acquire)) {
                const auto recoveryDelay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::milliseconds(8));
                retryDelay = retryDelay ? (std::min)(*retryDelay, recoveryDelay) : recoveryDelay;
            }
			if (drainTrace && now >= nextTracePopulationSnapshot) {
				nextTracePopulationSnapshot = now + std::chrono::milliseconds(250);
				tracePopulation = AsyncStateGraphTracePayload{ { nodes.size(), versions.size(),
					pending.size(), completionCount.load(std::memory_order_relaxed),
					gpuRecovery.size(), exactWaiterCount } };
			}
			const bool publishDiagnostics = basic_telemetry::Enabled() &&
				now >= nextDiagnosticSnapshot;
			if (publishDiagnostics) {
				nextDiagnosticSnapshot = now + std::chrono::milliseconds(250);
				// Detailed retention and per-kind scans used to run here while the
				// graph mutex was held. At radius 100 that diagnostic-only work
				// extended otherwise small mutation sections into global stalls.
				// Capture only O(1), mutation-owned gauges; the unified trace
				// reconstructs detailed populations after capture.
				stats.exactWaiters = exactWaiterCount;
				diagnosticSnapshot = DiagnosticSnapshot{ nodes.size(), versions.size(),
					reclaimedVersions, exactWaiterCount, pending.size(), stats.gpuWaiting };
            }
			recordApplyPhase("apply_tail");
        }
		{
			auto reclaimLock = LockMutex(GraphMutexPhase::Reclaim);
			ReclaimUnreferencedVersions(std::chrono::steady_clock::now() +
				std::chrono::milliseconds(1));
			while (!pendingRetirement.empty()) {
				retiredVersions.push_back(std::move(pendingRetirement.front()));
				pendingRetirement.pop_front();
				if (std::chrono::steady_clock::now() - started >= maxDuration) break;
			}
			hasImmediateWork = hasImmediateWork || !pendingRetirement.empty() ||
				!reclaimQueue.empty();
		}
		if (diagnosticSnapshot) {
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.Nodes",
				static_cast<std::int64_t>(diagnosticSnapshot->nodes));
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.ArchivedVersions",
				static_cast<std::int64_t>(diagnosticSnapshot->versions));
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.ReclaimedVersionsTotal",
				static_cast<std::int64_t>(diagnosticSnapshot->reclaimed));
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.ExactWaiters",
				static_cast<std::int64_t>(diagnosticSnapshot->exactWaiters));
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.Pending",
				static_cast<std::int64_t>(diagnosticSnapshot->pending));
			basic_telemetry::SetGauge("SARP.AsyncStateGraph.GpuWaiting",
				static_cast<std::int64_t>(diagnosticSnapshot->gpuWaiting));
		}
		for (const auto value : buildLatencySamples)
			basic_telemetry::Record("SARP.AsyncStateGraph.BuildLatencyNs", value);
		for (const auto value : gpuLatencySamples)
			basic_telemetry::Record("SARP.AsyncStateGraph.GpuLatencyNs", value);
		for (const auto value : queueLatencySamples)
			basic_telemetry::Record("SARP.AsyncStateGraph.QueueLatencyNs", value);
		if (staleCompletionTelemetry != 0)
			basic_telemetry::AddCounter("SARP.AsyncStateGraph.StaleCompletions",
				static_cast<std::int64_t>(staleCompletionTelemetry));
		if (drainTrace && tracePopulation)
            drainTrace->Record(AsyncStateGraphTraceEventID::GraphPopulation, {}, 0, 0,
                ArtifactReadiness::Missing, 0, *tracePopulation);
        // Immediate follow-up work is a level-triggered mailbox notification.
        // If it arrives while this drain owns the consumer, the same worker
        // loops; otherwise the notifying producer schedules the successor.
        hasImmediateWork = hasImmediateWork || !gpuSignals.empty();
		for (auto& subscription : pendingGpuSubscriptions) {
			if (subscription.token && subscription.token->subscribe) {
				auto weak = weak_from_this();
				const auto key = subscription.key;
				subscription.token->subscribe([weak, key] {
					if (auto self = weak.lock()) {
						self->gpuSignals.push({ key });
						self->ScheduleDrain();
					}
				});
			}
			// Close both the completion-before-subscribe and submission-before-
			// subscribe races. The graph's generation/token checks make duplicates safe.
			gpuSignals.push({ subscription.key });
		}
		hasImmediateWork = hasImmediateWork || !pendingGpuSubscriptions.empty();
        for (auto& [registration, context, superseded] : builds)
            SubmitBuild(registration, std::move(context), std::move(superseded));
        EnqueueAcceptances(std::move(acceptanceDispatches));
		for (auto& [action, snapshot] : accepted) if (action) action(snapshot);
		for (auto& [waiter, snapshot] : exactDispatches) {
			if (!waiter.continuation) continue;
			if (auto session = AcquireTrace()) {
				session->Record(AsyncStateGraphTraceEventID::ExactWaitSatisfied, waiter.version.address,
					waiter.version.revision, waiter.version.generation, snapshot.readiness,
					0, { { waiter.subscription, 0 } });
			}
			auto continuation = std::make_shared<std::function<void(const ArtifactSnapshot&)>>(
				std::move(waiter.continuation));
			const bool submitted = scheduler.Submit(scope, waiter.lane, waiter.domain,
				"AsyncStateGraph::AwaitExact",
				[continuation, snapshot](
					const TaskContext& context) mutable {
					if (!context.StopRequested()) (*continuation)(snapshot);
				});
			if (!submitted) (*continuation)(snapshot);
		}
        for (const auto& callback : callbacks) {
            if (callback) for (const auto& snapshot : ready) callback(snapshot);
        }
		if (!retiredVersions.empty()) {
			retiredVersions.clear();
			// Artifact destruction is complete and no graph mutex is held. Wake
			// capacity waiters that may now reuse an unpublished backing.
			NotifyVersionedGpuBufferFrameRetirement();
		}
        if (drainTrace) {
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            drainTrace->Record(AsyncStateGraphTraceEventID::DrainCompleted, {}, 0, 0, ArtifactReadiness::Missing,
                duration, { { transitions, builds.size(), ready.size(), signalledGpu.size() } });
        }
        if (hasImmediateWork) ScheduleDrain();
        else if (retryDelay) ScheduleDrain((std::max)(*retryDelay,
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(1))));
    }
};

AsyncStateGraph::AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name)
    : m_impl(std::make_shared<Impl>(scheduler, name)) {
    m_impl->ConfigureDrainPump();
}

AsyncStateGraph::~AsyncStateGraph() { Shutdown(); }

// Request replacement can release the last graph-owned reference to a large
// immutable recipe, checkpoint, or completed payload. Their destructors may in
// turn release substantial CPU/GPU ownership trees. Keep those releases out of
// the graph mutex, and share one sink across a request batch so none of the old
// state is destroyed between mutations.
struct AsyncStateGraph::RequestDeferredCleanup {
    struct AcceptedInputTrace {
		ArtifactKey key{};
		std::uint64_t revision = 0;
		std::uint64_t generation = 0;
		ArtifactPayload input;
	};
    std::vector<ArtifactPayload> payloads;
    std::vector<std::vector<ArtifactRequirement>> requirements;
    std::vector<std::vector<ArtifactSnapshot>> snapshots;
    std::vector<ArtifactLease> leases;
    std::vector<std::shared_ptr<const GpuSubmissionSet>> submissions;
	std::vector<std::shared_ptr<std::atomic_bool>> supersededTokens;
	std::vector<std::string> errors;
	std::vector<AcceptedInputTrace> acceptedInputs;

    void Reserve(std::size_t requestCount) {
        payloads.reserve(requestCount * 3);
        requirements.reserve(requestCount * 2);
        snapshots.reserve(requestCount);
        leases.reserve(requestCount);
        submissions.reserve(requestCount * 2);
		supersededTokens.reserve(requestCount);
		errors.reserve(requestCount);
		acceptedInputs.reserve(requestCount);
    }
};

void AsyncStateGraph::Impl::FlushDeferredRequestTrace(
	RequestDeferredCleanup& cleanup) {
	if (cleanup.acceptedInputs.empty()) return;
	auto requestTrace = AcquireTrace();
	if (!requestTrace) {
		cleanup.acceptedInputs.clear();
		return;
	}
	for (const auto& deferred : cleanup.acceptedInputs) {
		if (deferred.key.kind == ArtifactKind::StaticTransaction) {
			if (const auto transaction = deferred.input.Get<StaticTransactionBuildInput>()) {
				requestTrace->Record(AsyncStateGraphTraceEventID::StaticTransactionContents,
					deferred.key, deferred.revision, deferred.generation,
					ArtifactReadiness::Missing, 0,
					{ { transaction->groupCount, transaction->placementCount,
						transaction->drawRecordCount, transaction->activeEntryCount,
						transaction->streamGeneration } });
				for (const auto& group : transaction->groups) {
					requestTrace->Record(AsyncStateGraphTraceEventID::StaticGroupTransactionLinked,
						deferred.key, deferred.revision, deferred.generation,
						ArtifactReadiness::Missing, 0,
						{ { group.placementCount, group.drawRecordCount,
							group.activeEntryCount } },
						{ ArtifactKind::StaticGroup, group.groupID, 0 },
						transaction->streamGeneration);
				}
			}
		} else if (deferred.key.kind == ArtifactKind::StaticScene) {
			if (const auto scene = deferred.input.Get<StaticSceneBuildInput>()) {
				requestTrace->Record(AsyncStateGraphTraceEventID::StaticSceneContents,
					deferred.key, deferred.revision, deferred.generation,
					ArtifactReadiness::Missing, 0,
					{ { scene->pages.size(), scene->desiredPlacementCount,
						scene->materializedPlacementCount, scene->retiredPlacementCount } });
			}
		}
	}
	cleanup.acceptedInputs.clear();
}

// Request recipe copies and cancellation state may allocate. Construct them
// before entering the graph mutex so a large batch does not serialize heap
// work with unrelated graph transitions.
struct AsyncStateGraph::RequestPreparedState {
    std::vector<ArtifactRequirement> requestedRequirements;
    std::vector<ArtifactRequirement> signatureRequirements;
    std::shared_ptr<std::atomic_bool> superseded;

    explicit RequestPreparedState(const std::vector<ArtifactRequirement>& requirements)
        : requestedRequirements(requirements),
          signatureRequirements(requirements),
          superseded(std::make_shared<std::atomic_bool>(false)) {}
};

void AsyncStateGraph::RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration) {
    auto lock = m_impl->LockMutex(GraphMutexPhase::RegisterProducer);
    m_impl->producers[kind] = std::move(registration);
}

ArtifactRequestResult AsyncStateGraph::Request(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t requestFingerprint) {
    RequestDeferredCleanup cleanup;
	cleanup.Reserve(1);
    RequestPreparedState prepared(requirements);
	auto result = RequestInternal(key, desiredRevision, std::move(requirements), std::move(input),
		requestFingerprint, false, false, &cleanup, &prepared);
	m_impl->FlushDeferredRequestTrace(cleanup);
	return result;
}

ArtifactRequestStatus AsyncStateGraph::SubmitLatestIntent(ArtifactKey key,
    std::uint64_t desiredRevision, std::vector<ArtifactRequirement> requirements,
    ArtifactPayload input, std::uint64_t requestFingerprint) {
    if (key.kind == ArtifactKind::StaticTransaction) {
        return ArtifactRequestStatus::TypeMismatch;
    }
    RequestDeferredCleanup cleanup;
	cleanup.Reserve(1);
    RequestPreparedState prepared(requirements);
	auto result = RequestInternal(key, desiredRevision, std::move(requirements), std::move(input),
		requestFingerprint, true, false, &cleanup, &prepared);
	m_impl->FlushDeferredRequestTrace(cleanup);
	return result.status;
}

std::vector<ArtifactRequestResult> AsyncStateGraph::SubmitLatestIntentBatch(
    std::vector<ArtifactIntent> intents) {
    std::vector<ArtifactRequestResult> results(intents.size());
    std::vector<bool> valid(intents.size(), true);
    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> uniqueAddresses;
    std::unordered_set<Impl::VersionKey, Impl::VersionKey::Hasher> uniqueVersions;
    uniqueAddresses.reserve(intents.size());
    uniqueVersions.reserve(intents.size());
    RequestDeferredCleanup cleanup;
    cleanup.Reserve(intents.size());
    std::vector<RequestPreparedState> prepared;
    prepared.reserve(intents.size());
    for (const auto& intent : intents) prepared.emplace_back(intent.requirements);

    // Do immutable validation and canonical hashing before taking the graph
    // mutex. Keep one result slot per input intent: duplicate addresses still
    // execute in caller order because latest-wins semantics are observable.
    for (std::size_t index = 0; index < intents.size(); ++index) {
        auto& intent = intents[index];
        if (intent.key.kind == ArtifactKind::StaticTransaction) {
            results[index] = { ArtifactRequestStatus::TypeMismatch, 0, {} };
            valid[index] = false;
            continue;
        }
        if (intent.input.Valid() && intent.requestFingerprint == 0) {
            results[index] = { ArtifactRequestStatus::MissingFingerprint, 0, {} };
            valid[index] = false;
            continue;
        }
        if (intent.requestFingerprint == 0) {
            intent.requestFingerprint = CanonicalRequestFingerprint(intent.key,
                intent.desiredRevision, intent.requirements);
        }
        uniqueAddresses.insert(intent.key);
        uniqueVersions.insert({ intent.key, intent.desiredRevision });
    }

    const auto reserveForBatch = [](auto& container, std::size_t additional) {
        const auto required = container.size() + additional;
        const auto currentCapacity = static_cast<std::size_t>(
            container.bucket_count() * container.max_load_factor());
        if (required > currentCapacity) {
            container.reserve((std::max)(required,
                container.size() + container.size() / 2 + std::size_t{ 16 }));
        }
    };
    {
		auto reserveLock = m_impl->LockMutex(GraphMutexPhase::RequestBatch);
		++m_impl->stats.intentBatches;
		reserveForBatch(m_impl->nodes, uniqueAddresses.size());
		reserveForBatch(m_impl->versionGenerations, uniqueVersions.size());
		reserveForBatch(m_impl->versionSignatures, uniqueVersions.size());
	}
	constexpr std::size_t maxMutationsPerSlice = 16;
	constexpr auto maxMutationSlice = std::chrono::microseconds(500);
	for (std::size_t index = 0; index < intents.size();) {
		auto batchLock = m_impl->LockMutex(GraphMutexPhase::RequestBatch);
		const auto deadline = std::chrono::steady_clock::now() + maxMutationSlice;
		std::size_t applied = 0;
		do {
			if (valid[index]) {
				auto& intent = intents[index];
				results[index] = RequestInternal(intent.key, intent.desiredRevision,
					std::move(intent.requirements), std::move(intent.input),
					intent.requestFingerprint, true, true, &cleanup, &prepared[index]);
			}
			++index;
			++applied;
		} while (index < intents.size() && applied < maxMutationsPerSlice &&
			std::chrono::steady_clock::now() < deadline);
    }
	m_impl->FlushDeferredRequestTrace(cleanup);
    m_impl->ScheduleDrain();
    return results;
}

std::vector<ArtifactRequestResult> AsyncStateGraph::RequestBatch(
    std::vector<ArtifactRequest> requests) {
    std::vector<ArtifactRequestResult> results;
    results.reserve(requests.size());
    RequestDeferredCleanup cleanup;
    cleanup.Reserve(requests.size());
    std::vector<RequestPreparedState> prepared;
    prepared.reserve(requests.size());
    for (const auto& request : requests) prepared.emplace_back(request.requirements);
	constexpr std::size_t maxMutationsPerSlice = 64;
	constexpr auto maxMutationSlice = std::chrono::milliseconds(1);
	for (std::size_t index = 0; index < requests.size();) {
		auto batchLock = m_impl->LockMutex(GraphMutexPhase::RequestBatch);
		const auto deadline = std::chrono::steady_clock::now() + maxMutationSlice;
		std::size_t applied = 0;
		do {
			auto& request = requests[index];
			results.push_back(RequestInternal(request.key, request.desiredRevision,
				std::move(request.requirements), std::move(request.input),
				request.requestFingerprint, false, true, &cleanup, &prepared[index]));
			++index;
			++applied;
		} while (index < requests.size() && applied < maxMutationsPerSlice &&
			std::chrono::steady_clock::now() < deadline);
    }
	m_impl->FlushDeferredRequestTrace(cleanup);
    m_impl->ScheduleDrain();
    return results;
}

ArtifactRequestResult AsyncStateGraph::RequestInternal(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t requestFingerprint, bool coalescibleIntent, bool callerOwnsMutex,
    RequestDeferredCleanup* deferredCleanup, RequestPreparedState* preparedState) {
    RequestDeferredCleanup localCleanup;
    if (!deferredCleanup) deferredCleanup = &localCleanup;
    std::optional<RequestPreparedState> localPrepared;
    if (!preparedState) {
        localPrepared.emplace(requirements);
        preparedState = &*localPrepared;
    }
    auto requestTrace = m_impl->AcquireTrace();
    if (requestTrace) {
        requestTrace->Record(AsyncStateGraphTraceEventID::RequestReceived, key, desiredRevision, 0,
            ArtifactReadiness::Missing, 0,
            { { requirements.size(), requestFingerprint } });
    }
    if (m_impl->shuttingDown.load(std::memory_order_acquire)) {
        return { ArtifactRequestStatus::ShuttingDown, 0 };
    }
    ArtifactRequestResult result;
    {
        Impl::TimedMutexLock lock(*m_impl, GraphMutexPhase::Request, !callerOwnsMutex);
        auto requestPhaseStarted = requestTrace
            ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const auto recordRequestPhase = [&](std::string_view phase) {
            if (!requestTrace) return;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                now - requestPhaseStarted).count();
            requestPhaseStarted = now;
            if (elapsed < 2'000) return;
            requestTrace->Record(AsyncStateGraphTraceEventID::RequestPhase, key, desiredRevision, 0,
                ArtifactReadiness::Missing, elapsed, { { StableTraceID(phase) } });
        };
        const auto producer = m_impl->producers.find(key.kind);
        if (producer != m_impl->producers.end() &&
            producer->second.inputType != std::type_index(typeid(void)) &&
            (!input.Valid() || input.Type() != producer->second.inputType)) {
            return { ArtifactRequestStatus::TypeMismatch, 0 };
        }
        if (input.Valid() && requestFingerprint == 0) {
            return { ArtifactRequestStatus::MissingFingerprint, 0 };
        }
        if (requestFingerprint == 0) {
            requestFingerprint = CanonicalRequestFingerprint(key, desiredRevision, requirements);
        }
        if (coalescibleIntent) {
            const auto kindIndex = static_cast<std::size_t>(key.kind);
            if (kindIndex < m_impl->stats.intentsByKind.size())
                ++m_impl->stats.intentsByKind[kindIndex];
        }
        const Impl::VersionKey requestedVersion{ key, desiredRevision };
        auto versionGeneration = m_impl->versionGenerations.find(requestedVersion);
        if (versionGeneration == m_impl->versionGenerations.end()) {
            versionGeneration = m_impl->versionGenerations.emplace(
                requestedVersion, ++m_impl->nextVersionGeneration).first;
        }
        const auto traceAcceptedRequest = [&] {
            if (!requestTrace) return;
            for (const auto& requirement : requirements) {
                if (!requestTrace->Config().includeDependencyEvents) break;
                requestTrace->Record(AsyncStateGraphTraceEventID::DependencyDeclared, key, desiredRevision,
                    versionGeneration->second, requirement.requiredReadiness, 0,
                    { { static_cast<unsigned>(requirement.policy),
                        static_cast<unsigned>(requirement.invalidation),
                        requirement.alternativeGroup, requirement.requiredGeneration } },
                    requirement.key, requirement.minimumRevision);
            }
			if (key.kind == ArtifactKind::StaticTransaction ||
				key.kind == ArtifactKind::StaticScene) {
				deferredCleanup->acceptedInputs.push_back({ key, desiredRevision,
					versionGeneration->second, input });
            }
        };
		auto [nodeEntry, insertedNode] = m_impl->nodes.try_emplace(key);
		auto& node = nodeEntry->second;
		if (insertedNode) ++m_impl->nodeStateCounts[
			static_cast<std::size_t>(ArtifactReadiness::Missing)];
        recordRequestPhase("validate_and_lookup");
        node.key = key;
		m_impl->SetDesired(node, true);
        const auto knownSignature = m_impl->versionSignatures.find(requestedVersion);
        if (knownSignature != m_impl->versionSignatures.end()) {
            if (knownSignature->second.fingerprint != requestFingerprint ||
                knownSignature->second.inputType != input.Type() ||
                knownSignature->second.requirements != requirements) {
                if (requestTrace) {
                    requestTrace->Record(AsyncStateGraphTraceEventID::RequestConflict, key, desiredRevision,
                        versionGeneration->second, node.state);
                }
                return { ArtifactRequestStatus::ConflictingRevision, node.generation,
                    { key, desiredRevision, versionGeneration->second } };
            }
            const bool stillDesired = node.desired && (
                node.desiredRevision == desiredRevision ||
                std::ranges::any_of(node.successors, [&](const Impl::RequestedVersion& successor) {
                    return successor.revision == desiredRevision;
                }));
            if (!stillDesired && desiredRevision < node.latestRequestedRevision) {
                return { ArtifactRequestStatus::StaleRevision, node.generation,
                    { key, desiredRevision, versionGeneration->second } };
            }
            if (stillDesired) {
                auto versionLease = m_impl->AcquireVersionLease({ key, desiredRevision,
                    versionGeneration->second });
                if (requestTrace) {
                    requestTrace->Record(AsyncStateGraphTraceEventID::RequestAlreadyDesired, key, desiredRevision,
                        versionGeneration->second, node.state);
                }
                return { ArtifactRequestStatus::AlreadyDesired, node.generation,
                    { key, desiredRevision, versionGeneration->second }, versionLease };
            }
            // A signature is only a conflict-detection tombstone. Release may
            // have removed the address node, so an identical request must
            // recreate desired state rather than pretending that the version
            // is still desired. Preserve the immutable generation while an
            // archived copy exists; otherwise assign a new ABA generation.
            const bool archived = std::ranges::any_of(m_impl->versions,
                [&](const auto& entry) {
                    return entry.first.address == key &&
                        entry.first.revision == desiredRevision &&
                        entry.first.generation == versionGeneration->second;
                });
            if (!archived) {
                versionGeneration->second = ++m_impl->nextVersionGeneration;
            }
        }
        else if (node.latestRequestedRevision != 0 && desiredRevision < node.latestRequestedRevision) {
            return { ArtifactRequestStatus::StaleRevision, node.generation,
                { key, desiredRevision, versionGeneration->second } };
        }
        auto versionLease = m_impl->AcquireVersionLease({ key, desiredRevision,
            versionGeneration->second });
        if (knownSignature == m_impl->versionSignatures.end()) {
            m_impl->versionSignatures.emplace(requestedVersion, Impl::VersionSignature{
                requestFingerprint, input.Type(),
                std::move(preparedState->signatureRequirements) });
        }
        node.latestRequestedRevision = desiredRevision;
        ++m_impl->stats.requests;

        const bool activeVersionExists = node.desiredRevision != 0;
        const bool activeVersionFinished = activeVersionExists &&
            node.producedRevision == node.desiredRevision &&
            (node.state == ArtifactReadiness::GpuReady ||
             node.state == ArtifactReadiness::Published);
        if (activeVersionExists && !activeVersionFinished) {
            traceAcceptedRequest();
            if (coalescibleIntent) {
                const Impl::StoredVersionKey activeVersion{
                    key, node.desiredRevision, node.versionGeneration };
                // Latest-wins cancellation is only legal while no immutable
                // transaction has pinned this exact generation. Once pinned,
                // finish it and coalesce only the queued successors; otherwise
                // exact consumers can be stranded on a Superseded version that
                // never had a chance to materialize.
                if (!m_impl->HasExactDependent(activeVersion) && node.superseded &&
                    !node.superseded->exchange(true, std::memory_order_acq_rel))
                    ++m_impl->stats.supersededBuilds;
                while (!node.successors.empty() && node.successors.back().coalescibleIntent) {
                    const auto& candidate = node.successors.back();
                    const Impl::StoredVersionKey candidateVersion{
                        key, candidate.revision, candidate.generation };
                    if (m_impl->HasExactDependent(candidateVersion)) break;
                    m_impl->UnpinSuccessor(node, node.successors.back());
                    node.successors.pop_back();
                    ++m_impl->stats.coalescedIntents;
                    const auto kindIndex = static_cast<std::size_t>(key.kind);
                    if (kindIndex < m_impl->stats.coalescedByKind.size())
                        ++m_impl->stats.coalescedByKind[kindIndex];
                }
            }
            node.successors.push_back({ desiredRevision, versionGeneration->second, requestFingerprint,
                std::move(input), std::move(requirements),
                std::move(preparedState->requestedRequirements), versionLease,
                coalescibleIntent });
            m_impl->PinSuccessor(node, node.successors.back());
            const Impl::StoredVersionKey activeVersion{
                key, node.desiredRevision, node.versionGeneration };
            const bool obsoleteBeforeBuild = coalescibleIntent && !node.buildInFlight &&
                !m_impl->HasExactDependent(activeVersion) &&
                (node.state == ArtifactReadiness::Missing ||
                 node.state == ArtifactReadiness::Blocked ||
                 node.state == ArtifactReadiness::Queued);
            // A blocked latest-wins version cannot reach UploadSubmitted to
            // trigger the normal successor handoff. If nobody pins it, replace
            // it immediately; otherwise a superseded exact dependency can
            // strand the address forever while newer mailbox state accumulates.
            if (obsoleteBeforeBuild ||
                (node.state == ArtifactReadiness::UploadSubmitted && !node.buildInFlight)) {
                m_impl->PromoteSuccessor(node);
            }
            result = { ArtifactRequestStatus::Accepted, node.generation,
                { key, desiredRevision, versionGeneration->second }, versionLease };
            if (requestTrace) {
                requestTrace->Record(AsyncStateGraphTraceEventID::SuccessorQueued, key, desiredRevision,
                    versionGeneration->second, node.state);
            }
            // The active immutable version retains its own input and closure.
            if (!callerOwnsMutex) m_impl->ScheduleDrain();
            return result;
        }

        m_impl->StoreVersion(node);
        traceAcceptedRequest();
        recordRequestPhase("store_and_trace_request");
        m_impl->RemoveWaiterEdges(node);
        recordRequestPhase("remove_waiter_edges");
        node.desiredRevision = desiredRevision;
        node.versionGeneration = versionGeneration->second;
        node.generation = versionGeneration->second;
        node.requestFingerprint = requestFingerprint;
        deferredCleanup->leases.push_back(std::move(node.lease));
        deferredCleanup->requirements.push_back(std::move(node.requirements));
        deferredCleanup->requirements.push_back(std::move(node.requestedRequirements));
        deferredCleanup->payloads.push_back(std::move(node.input));
        deferredCleanup->payloads.push_back(std::move(node.payload));
        deferredCleanup->payloads.push_back(std::move(node.checkpoint));
        deferredCleanup->snapshots.push_back(std::move(node.resolvedDependencies));
		node.resolvedDependencyIndex.clear();
        deferredCleanup->submissions.push_back(std::move(node.gpuSubmissions));
        deferredCleanup->submissions.push_back(std::move(node.waitingGpuSubmissions));
        recordRequestPhase("defer_old_version");
        node.lease = versionLease;
        node.requirements = std::move(requirements);
        node.requestedRequirements = std::move(preparedState->requestedRequirements);
        node.input = std::move(input);
		recordRequestPhase("install_new_recipe");
        node.producedRevision = 0;
        node.payload = {};
        node.checkpoint = {};
        node.resolvedDependencies.clear();
		node.resolvedDependencyIndex.clear();
		recordRequestPhase("clear_node_state");
        node.terminalFailure = false;
        node.latestSuccessorNeeded = false;
        node.coalescibleIntent = coalescibleIntent;
		if (node.superseded) deferredCleanup->supersededTokens.push_back(
			std::move(node.superseded));
        node.superseded = std::move(preparedState->superseded);
        node.buildAttempted = false;
		if (!node.error.empty()) deferredCleanup->errors.push_back(std::move(node.error));
		recordRequestPhase("reset_control_state");
        node.gpuSubmissions.reset();
        node.waitingGpuSubmissions.reset();
		recordRequestPhase("reset_gpu_state");
        node.retryAt.reset();
        m_impl->SetState(node, ArtifactReadiness::Missing);
        recordRequestPhase("reset_version_state");
        m_impl->InstallWaiterEdges(node);
        recordRequestPhase("install_waiter_edges");
        const auto cycle = m_impl->DetectCycle(node);
        recordRequestPhase("detect_cycle");
        if (!cycle.empty()) {
            m_impl->FailCycle(cycle);
        } else {
            m_impl->QueueNode(node);
        }
        recordRequestPhase("queue_and_supersede");
        result = { ArtifactRequestStatus::Accepted, node.generation,
            { key, desiredRevision, versionGeneration->second }, versionLease };
        if (requestTrace) {
            requestTrace->Record(AsyncStateGraphTraceEventID::RequestAccepted, key, desiredRevision,
                versionGeneration->second, node.state);
        }
    }
    if (!callerOwnsMutex) m_impl->ScheduleDrain();
    return result;
}

ArtifactRequestResult AsyncStateGraph::RequestExpressions(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<DependencyExpression> dependencies, ArtifactPayload input,
    std::uint64_t requestFingerprint) {
    std::vector<ArtifactRequirement> requirements;
    std::uint32_t nextAlternativeGroup = 1;
    for (const auto& expression : dependencies) {
        std::visit([&](const auto& value) {
            using Expression = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Expression, Require>) {
                requirements.push_back({ value.requirement.key, value.requirement.minimumRevision,
                    value.requirement.requiredReadiness, DependencyPolicy::AllOf, 0 });
            } else if constexpr (std::is_same_v<Expression, Optional>) {
                requirements.push_back({ value.requirement.key, value.requirement.minimumRevision,
                    value.requirement.requiredReadiness, DependencyPolicy::Optional, 0 });
            } else {
                const auto policy = std::is_same_v<Expression, FirstReady>
                    ? DependencyPolicy::FallbackAllowed : DependencyPolicy::AnyOf;
                const auto group = nextAlternativeGroup++;
                for (const auto& alternative : value.alternatives) {
                    requirements.push_back({ alternative.key, alternative.minimumRevision,
                        alternative.requiredReadiness, policy, group });
                }
            }
        }, expression);
    }
    return Request(key, desiredRevision, std::move(requirements), std::move(input),
        requestFingerprint);
}

bool AsyncStateGraph::Invalidate(ArtifactKey key, std::uint64_t desiredRevision) {
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(AsyncStateGraphTraceEventID::Invalidated, key, desiredRevision);
    }
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    bool foundNode = false;
    std::uint64_t requestFingerprint = 0;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::Invalidate);
        const auto found = m_impl->nodes.find(key);
        foundNode = found != m_impl->nodes.end();
        if (foundNode) {
            if (!found->second.successors.empty()) {
                const auto& latest = found->second.successors.back();
                requirements = latest.requirements;
                input = latest.input;
                requestFingerprint = latest.fingerprint;
            } else {
                requirements = found->second.requirements;
                input = found->second.input;
                requestFingerprint = found->second.requestFingerprint;
            }
        }
        ++m_impl->stats.invalidations;
    }
    return static_cast<bool>(Request(key, desiredRevision,
        foundNode ? std::move(requirements) : std::vector<ArtifactRequirement>{}, std::move(input),
        requestFingerprint));
}

void AsyncStateGraph::Cancel(ArtifactKey key) {
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(AsyncStateGraphTraceEventID::Cancelled, key);
    }
	std::shared_ptr<const GpuSubmissionSet> cancellation;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::CancelApply);
        const auto found = m_impl->nodes.find(key);
        if (found == m_impl->nodes.end()) return;
        m_impl->RemoveWaiterEdges(found->second);
        ++found->second.generation;
		m_impl->SetDesired(found->second, false);
        m_impl->ClearSuccessors(found->second);
        found->second.retryAt.reset();
        if ((found->second.state == ArtifactReadiness::CpuReady ||
             found->second.state == ArtifactReadiness::UploadSubmitted) &&
            found->second.waitingGpuSubmissions &&
            m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
		cancellation = std::move(found->second.waitingGpuSubmissions);
        found->second.gpuSubmissions.reset();
        found->second.lease.reset();
        m_impl->SetState(found->second, ArtifactReadiness::Cancelled);
        ++m_impl->stats.cancelled;
    }
	if (cancellation) (void)cancellation->Cancel();
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::Release(ArtifactKey key) {
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(AsyncStateGraphTraceEventID::Released, key);
    }
	std::shared_ptr<const GpuSubmissionSet> cancellation;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::CancelApply);
        const auto found = m_impl->nodes.find(key);
        if (found == m_impl->nodes.end()) return;
        auto& node = found->second;
        m_impl->RemoveWaiterEdges(node);
        ++node.generation;
		m_impl->SetDesired(node, false);
        m_impl->ClearSuccessors(node);
        node.retryAt.reset();
        if ((node.state == ArtifactReadiness::CpuReady ||
             node.state == ArtifactReadiness::UploadSubmitted) && node.waitingGpuSubmissions &&
            m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
		cancellation = std::move(node.waitingGpuSubmissions);
        node.gpuSubmissions.reset();
        node.lease.reset();
        m_impl->SetState(node, ArtifactReadiness::Cancelled);
        ++m_impl->stats.cancelled;

        const auto dependents = m_impl->waiters.find(key);
        if (!node.buildInFlight &&
			(dependents == m_impl->waiters.end() || dependents->second.empty())) {
			const auto stateIndex = static_cast<std::size_t>(node.state);
			if (stateIndex < m_impl->nodeStateCounts.size() &&
				m_impl->nodeStateCounts[stateIndex] != 0)
				--m_impl->nodeStateCounts[stateIndex];
			m_impl->nodes.erase(found);
		}
    }
	if (cancellation) (void)cancellation->Cancel();
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::ReleaseBatch(std::span<const ArtifactKey> keys) {
    if (keys.empty()) return;

    std::vector<ArtifactKey> uniqueKeys;
    uniqueKeys.reserve(keys.size());
    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> seen;
    seen.reserve(keys.size());
    for (const auto& key : keys) {
        if (seen.insert(key).second) uniqueKeys.push_back(key);
    }
    if (auto session = m_impl->AcquireTrace()) {
        for (const auto& key : uniqueKeys)
            session->Record(AsyncStateGraphTraceEventID::Released, key);
    }

    std::vector<std::shared_ptr<const GpuSubmissionSet>> cancellations;
    cancellations.reserve(uniqueKeys.size());
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::CancelApply);
        for (const auto& key : uniqueKeys) {
            const auto found = m_impl->nodes.find(key);
            if (found == m_impl->nodes.end()) continue;
            auto& node = found->second;
            m_impl->RemoveWaiterEdges(node);
            ++node.generation;
			m_impl->SetDesired(node, false);
            m_impl->ClearSuccessors(node);
            node.retryAt.reset();
            if ((node.state == ArtifactReadiness::CpuReady ||
                 node.state == ArtifactReadiness::UploadSubmitted) &&
                node.waitingGpuSubmissions && m_impl->stats.gpuWaiting)
                --m_impl->stats.gpuWaiting;
            if (node.waitingGpuSubmissions)
                cancellations.push_back(std::move(node.waitingGpuSubmissions));
            node.gpuSubmissions.reset();
            node.lease.reset();
            m_impl->SetState(node, ArtifactReadiness::Cancelled);
            ++m_impl->stats.cancelled;

            const auto dependents = m_impl->waiters.find(key);
            if (!node.buildInFlight &&
				(dependents == m_impl->waiters.end() || dependents->second.empty())) {
				const auto stateIndex = static_cast<std::size_t>(node.state);
				if (stateIndex < m_impl->nodeStateCounts.size() &&
					m_impl->nodeStateCounts[stateIndex] != 0)
					--m_impl->nodeStateCounts[stateIndex];
				m_impl->nodes.erase(found);
			}
        }
    }
    for (const auto& cancellation : cancellations)
        if (cancellation) (void)cancellation->Cancel();
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::MarkPublished(ArtifactKey key, std::uint64_t revision) {
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(AsyncStateGraphTraceEventID::Published, key, revision, 0, ArtifactReadiness::Published);
    }
	std::vector<Impl::StoredVersionKey> publishedVersions;
    auto lock = m_impl->LockMutex(GraphMutexPhase::MarkPublishedRevision);
	if (const auto address = m_impl->versionsByAddress.find(key);
		address != m_impl->versionsByAddress.end()) {
		for (auto entry = address->second.lower_bound({ revision, 0 });
			entry != address->second.end() && entry->first.first == revision; ++entry) {
			const auto archived = m_impl->versions.find(entry->second);
			if (archived == m_impl->versions.end() ||
				(archived->second.readiness != ArtifactReadiness::GpuReady &&
				 archived->second.readiness != ArtifactReadiness::Published)) continue;
			if (archived->second.readiness != ArtifactReadiness::Published) {
				archived->second.readiness = ArtifactReadiness::Published;
				publishedVersions.push_back(entry->second);
			}
		}
	}
    const auto found = m_impl->nodes.find(key);
    if (found != m_impl->nodes.end() && found->second.producedRevision == revision &&
        (found->second.state == ArtifactReadiness::UploadSubmitted ||
         found->second.state == ArtifactReadiness::GpuReady ||
         found->second.state == ArtifactReadiness::Published)) {
        found->second.published = true;
        if (found->second.state == ArtifactReadiness::GpuReady) {
            m_impl->SetState(found->second, ArtifactReadiness::Published);
        }
        m_impl->StoreVersion(found->second);
        m_impl->WakeWaiters(key);
    }
	lock.Unlock();
	for (const auto& version : publishedVersions) m_impl->publishedSignals.push(version);
	m_impl->ScheduleDrain();
}

void AsyncStateGraph::MarkPublished(ArtifactVersionID version) {
    const auto snapshot = Snapshot(version);
    if (snapshot.readiness == ArtifactReadiness::Missing ||
        snapshot.generation != version.generation) return;
    MarkPublished(version.address, version.revision);
}

void AsyncStateGraph::MarkPublished(std::span<const ArtifactVersionID> versions) {
    if (versions.empty()) return;
    auto lock = m_impl->LockMutex(GraphMutexPhase::MarkPublishedVersions);
    for (const auto& version : versions) {
        if (!version) continue;
        if (auto archived = m_impl->versions.find({ version.address, version.revision,
                version.generation });
            archived != m_impl->versions.end() &&
            archived->second.generation == version.generation &&
            (archived->second.readiness == ArtifactReadiness::UploadSubmitted ||
             archived->second.readiness == ArtifactReadiness::GpuReady ||
             archived->second.readiness == ArtifactReadiness::Published)) {
			if (archived->second.readiness != ArtifactReadiness::Published) {
				archived->second.readiness = ArtifactReadiness::Published;
				m_impl->publishedSignals.push({ version.address, version.revision, version.generation });
			}
        }
        const auto found = m_impl->nodes.find(version.address);
        if (found == m_impl->nodes.end() ||
            found->second.producedRevision != version.revision ||
            found->second.versionGeneration != version.generation)
            continue;
        auto& node = found->second;
        if (node.state != ArtifactReadiness::UploadSubmitted &&
            node.state != ArtifactReadiness::GpuReady &&
            node.state != ArtifactReadiness::Published) continue;
        node.published = true;
        if (node.state == ArtifactReadiness::GpuReady) {
            m_impl->SetState(node, ArtifactReadiness::Published);
        }
        m_impl->StoreVersion(node);
        m_impl->WakeWaiters(version.address);
    }
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::PumpGpuCompletions() {
    std::vector<ArtifactKey> pending;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::GpuCompletionScan);
        pending.reserve(m_impl->nodes.size());
        for (const auto& [key, node] : m_impl->nodes) {
            if ((node.state == ArtifactReadiness::CpuReady ||
                 node.state == ArtifactReadiness::UploadSubmitted) &&
                node.waitingGpuSubmissions) {
                pending.push_back(key);
            }
        }
    }
	for (const auto key : pending) m_impl->gpuSignals.push({ key });
    m_impl->ScheduleDrain();
}

std::uint64_t AsyncStateGraph::AllocateSuspensionIdentity() noexcept {
    auto identity = g_nextArtifactSuspensionIdentity.fetch_add(1, std::memory_order_relaxed);
    while (identity == 0) {
        identity = g_nextArtifactSuspensionIdentity.fetch_add(1, std::memory_order_relaxed);
    }
    return identity;
}

void AsyncStateGraph::NotifySuspensionSatisfied(std::uint64_t identity) {
    if (identity == 0) return;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::SuspensionSatisfied);
        const auto registered = m_impl->suspendedByIdentity.find(identity);
        if (registered == m_impl->suspendedByIdentity.end()) {
            // Level-triggered handshake: completion is retained if it races
            // ahead of Suspend() registration.
            m_impl->satisfiedSuspensions.insert(identity);
        } else {
            const auto version = registered->second;
            m_impl->suspendedByIdentity.erase(registered);
            const auto found = m_impl->nodes.find(version.address);
            if (found != m_impl->nodes.end() &&
                found->second.desiredRevision == version.revision &&
                found->second.generation == version.generation &&
                found->second.suspension &&
                found->second.suspension->identity == identity) {
                found->second.suspension.reset();
                if (auto session = m_impl->AcquireTrace()) {
                    session->Record(AsyncStateGraphTraceEventID::SuspensionSatisfied, version.address,
                        version.revision, version.generation, found->second.state);
                }
                m_impl->QueueNode(found->second);
            }
        }
    }
    m_impl->ScheduleDrain();
}

std::function<void(std::uint64_t)> AsyncStateGraph::MakeSuspensionNotifier() const {
    const std::weak_ptr<Impl> weak = m_impl;
    return [weak](std::uint64_t identity) {
        if (identity == 0) return;
        const auto impl = weak.lock();
        if (!impl) return;
        {
            auto lock = impl->LockMutex(GraphMutexPhase::SuspensionSatisfied);
            const auto registered = impl->suspendedByIdentity.find(identity);
            if (registered == impl->suspendedByIdentity.end()) {
                impl->satisfiedSuspensions.insert(identity);
            } else {
                const auto version = registered->second;
                impl->suspendedByIdentity.erase(registered);
                const auto found = impl->nodes.find(version.address);
                if (found != impl->nodes.end() &&
                    found->second.desiredRevision == version.revision &&
                    found->second.generation == version.generation &&
                    found->second.suspension &&
                    found->second.suspension->identity == identity) {
                    found->second.suspension.reset();
                    impl->QueueNode(found->second);
                }
            }
        }
        impl->ScheduleDrain();
    };
}

void AsyncStateGraph::SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback) {
    auto lock = m_impl->LockMutex(GraphMutexPhase::ReadyCallbackSet);
    if (callback) m_impl->readyCallbacks.insert_or_assign(0, std::move(callback));
    else m_impl->readyCallbacks.erase(0);
}

std::uint64_t AsyncStateGraph::AddReadyCallback(
    std::function<void(const ArtifactSnapshot&)> callback) {
    if (!callback) return 0;
    auto lock = m_impl->LockMutex(GraphMutexPhase::ReadyCallbackAdd);
    const auto subscription = ++m_impl->nextReadyCallback;
    m_impl->readyCallbacks.emplace(subscription, std::move(callback));
    return subscription;
}

void AsyncStateGraph::RemoveReadyCallback(std::uint64_t subscription) {
    if (subscription == 0) return;
    auto lock = m_impl->LockMutex(GraphMutexPhase::ReadyCallbackRemove);
    m_impl->readyCallbacks.erase(subscription);
}

ArtifactObservation AsyncStateGraph::ObserveWithSnapshot(ArtifactKey address,
    std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback) {
    auto sequence = std::make_shared<std::atomic_uint64_t>(0);
    const auto subscription = AddReadyCallback(
        [address, sequence, callback = std::move(callback)](const ArtifactSnapshot& snapshot) {
            if (snapshot.key != address) return;
            const auto next = sequence->fetch_add(1, std::memory_order_acq_rel) + 1;
            if (callback) callback(next, snapshot);
        });
    // Register first, then sample. An event racing between these operations is
    // observed either by the callback or by this level snapshot (usually both).
    auto snapshot = Snapshot(address);
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(AsyncStateGraphTraceEventID::ObservationRegistered, address, snapshot.revision,
            snapshot.generation, snapshot.readiness, 0,
            { { subscription } });
    }
    auto weak = std::weak_ptr<Impl>(m_impl);
    return { subscription, sequence->load(std::memory_order_acquire), std::move(snapshot),
        [weak, subscription] {
            if (subscription == 0) return;
            if (const auto graph = weak.lock()) {
                auto lock = graph->LockMutex(GraphMutexPhase::ObservationRemove);
                graph->readyCallbacks.erase(subscription);
                lock.Unlock();
                if (auto session = graph->AcquireTrace()) {
                    session->Record(AsyncStateGraphTraceEventID::ObservationCancelled, {}, 0, 0,
                        ArtifactReadiness::Missing, 0,
                        { { subscription } });
                }
                graph->ScheduleDrain();
            }
        } };
}

ArtifactObservation AsyncStateGraph::ObserveKind(ArtifactKind kind,
	std::function<void(std::uint64_t, const ArtifactSnapshot&)> callback) {
	auto sequence = std::make_shared<std::atomic_uint64_t>(0);
	const auto subscription = AddReadyCallback(
		[kind, sequence, callback = std::move(callback)](const ArtifactSnapshot& snapshot) {
			if (snapshot.key.kind != kind) return;
			const auto next = sequence->fetch_add(1, std::memory_order_acq_rel) + 1;
			if (callback) callback(next, snapshot);
		});
	auto weak = std::weak_ptr<Impl>(m_impl);
	if (auto session = m_impl->AcquireTrace()) {
		session->Record(AsyncStateGraphTraceEventID::KindObservationRegistered, { kind, 0, 0 }, 0, 0,
			ArtifactReadiness::Missing, 0, { { subscription } });
	}
	return { subscription, sequence->load(std::memory_order_acquire), {},
		[weak, subscription] {
			if (subscription == 0) return;
			if (const auto graph = weak.lock()) {
				auto lock = graph->LockMutex(GraphMutexPhase::ObservationRemove);
				graph->readyCallbacks.erase(subscription);
				lock.Unlock();
				if (auto session = graph->AcquireTrace()) {
					session->Record(AsyncStateGraphTraceEventID::ObservationCancelled, {}, 0, 0,
						ArtifactReadiness::Missing, 0,
						{ { subscription } });
				}
				graph->ScheduleDrain();
			}
		} };
}

ArtifactAwaiter AsyncStateGraph::AwaitExact(ArtifactVersionHandle handle,
	ArtifactReadiness milestone, TaskLane lane, TaskDomain domain,
	std::function<void(const ArtifactSnapshot&)> continuation) {
	if (!handle.version || !continuation ||
		m_impl->shuttingDown.load(std::memory_order_acquire)) return {};
	ArtifactSnapshot snapshot;
	std::uint64_t subscription = 0;
	bool dispatchNow = false;
	{
		auto lock = m_impl->LockMutex(GraphMutexPhase::ExactWaiterRegister);
		snapshot = m_impl->SnapshotExactLocked(handle.version);
		const bool terminal = snapshot.readiness == ArtifactReadiness::Failed ||
			snapshot.readiness == ArtifactReadiness::Cancelled ||
			snapshot.readiness == ArtifactReadiness::Superseded;
		dispatchNow = terminal || ArtifactReachedMilestone(snapshot.readiness, milestone);
		if (!dispatchNow) {
			subscription = ++m_impl->nextExactWaiter;
			m_impl->exactWaiters[{ handle.version.address, handle.version.revision,
				handle.version.generation }].push_back({
				subscription, handle.version, milestone, lane, domain,
					std::move(continuation), std::move(handle.lease) });
			++m_impl->exactWaiterCount;
		}
	}
	if (dispatchNow) {
		if (auto session = m_impl->AcquireTrace()) {
			session->Record(AsyncStateGraphTraceEventID::ExactWaitSatisfied, handle.version.address,
				handle.version.revision, handle.version.generation, snapshot.readiness,
				0, { { 0, 1 } });
		}
		auto callback = std::make_shared<std::function<void(const ArtifactSnapshot&)>>(
			std::move(continuation));
		const bool submitted = m_impl->scheduler.Submit(m_impl->scope, lane, domain,
			"AsyncStateGraph::AwaitExactReady",
			[callback, snapshot](const TaskContext& context) {
				if (!context.StopRequested()) (*callback)(snapshot);
			});
		if (!submitted) (*callback)(snapshot);
		return { 0, std::move(snapshot), {} };
	}
	if (auto session = m_impl->AcquireTrace()) {
		session->Record(AsyncStateGraphTraceEventID::ExactWaitRegistered, handle.version.address,
			handle.version.revision, handle.version.generation, snapshot.readiness,
			0, { { subscription, static_cast<unsigned>(milestone) } });
	}
	auto weak = std::weak_ptr<Impl>(m_impl);
	return { subscription, std::move(snapshot), [weak, subscription, version = handle.version] {
		if (subscription == 0) return;
		if (const auto graph = weak.lock()) {
			auto lock = graph->LockMutex(GraphMutexPhase::ExactWaiterRemove);
			const Impl::StoredVersionKey key{
				version.address, version.revision, version.generation };
			const auto found = graph->exactWaiters.find(key);
			if (found == graph->exactWaiters.end()) return;
			const auto removed = std::erase_if(found->second,
				[subscription](const Impl::ExactWaiter& waiter) {
					return waiter.subscription == subscription;
				});
			if (removed != 0 && graph->exactWaiterCount != 0)
				--graph->exactWaiterCount;
			if (found->second.empty()) graph->exactWaiters.erase(found);
			lock.Unlock();
			if (auto session = graph->AcquireTrace()) {
				session->Record(AsyncStateGraphTraceEventID::ExactWaitCancelled, version.address,
					version.revision, version.generation, ArtifactReadiness::Missing,
					0, { { subscription } });
			}
			graph->ScheduleDrain();
		}
	} };
}

ArtifactSnapshot AsyncStateGraph::Snapshot(ArtifactKey key) const {
    auto lock = m_impl->LockMutex(GraphMutexPhase::Snapshot);
    const auto found = m_impl->nodes.find(key);
    return found == m_impl->nodes.end() ? ArtifactSnapshot{ key } : m_impl->MakeSnapshot(found->second);
}

ArtifactSnapshot AsyncStateGraph::Snapshot(ArtifactVersionID version) const {
    auto lock = m_impl->LockMutex(GraphMutexPhase::Snapshot);
	return m_impl->SnapshotExactLocked(version);
}

ArtifactDiagnostic AsyncStateGraph::Diagnose(ArtifactKey key) const {
    auto lock = m_impl->LockMutex(GraphMutexPhase::Diagnose);
    auto diagnoseTrace = m_impl->AcquireTrace();
    auto phaseStarted = diagnoseTrace
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    const auto recordPhase = [&](std::string_view phase) {
        if (!diagnoseTrace) return;
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            now - phaseStarted).count();
        phaseStarted = now;
        if (elapsed < 2'000) return;
        diagnoseTrace->Record(AsyncStateGraphTraceEventID::DiagnosePhase, key, 0, 0, ArtifactReadiness::Missing,
            elapsed, { { StableTraceID(phase) } });
    };
    ArtifactDiagnostic result;
    const auto found = m_impl->nodes.find(key);
    if (found == m_impl->nodes.end()) { result.artifact.key = key; return result; }
    const auto& node = found->second;
    result.artifact = m_impl->MakeSnapshot(node);
    result.desiredRevision = node.latestRequestedRevision;
    result.error = node.error;
    result.stateAge = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - node.stateSince);
    for (const auto& requirement : node.requirements) {
        if (!m_impl->RequirementSatisfied(requirement) && requirement.policy != DependencyPolicy::Optional)
            result.blockers.push_back(requirement);
    }
    recordPhase("collect_direct_blockers");
    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
    m_impl->AppendBlockerChain(key, visited, result.blockerChain);
    recordPhase("append_blocker_chain");
    return result;
}

AsyncStateGraphStats AsyncStateGraph::Stats() const {
    auto lock = m_impl->LockMutex(GraphMutexPhase::Stats);
    auto result = m_impl->stats;
    result.archivedVersions = m_impl->versions.size();
    result.reclaimedVersions = m_impl->reclaimedVersions;
	result.stateCounts = m_impl->nodeStateCounts;
    return result;
}

std::uint64_t AsyncStateGraph::Outstanding(ArtifactKind kind) const {
    auto lock = m_impl->LockMutex(GraphMutexPhase::Outstanding);
	const auto index = static_cast<std::size_t>(kind);
	return index < m_impl->outstandingByKind.size()
		? m_impl->outstandingByKind[index] : 0;
}

void AsyncStateGraph::StartTrace(AsyncStateGraphTraceConfig config) {
    if (!m_impl || m_impl->shuttingDown.load(std::memory_order_acquire)) return;
    if (m_impl->trace.load(std::memory_order_acquire)) return;
    if (config.maximumEvents == 0) config.maximumEvents = 1;
    auto session = std::make_shared<GraphTraceSession>(config);
    session->Record(AsyncStateGraphTraceEventID::TraceStarted, {});
    m_impl->traceOwner = std::move(session);
    m_impl->trace.store(m_impl->traceOwner.get(), std::memory_order_release);
    (void)m_impl->scheduler.InstallTaskTraceSink(m_impl->traceOwner.get(),
        [](void* context, const TaskTraceEvent& event) noexcept {
            static_cast<GraphTraceSession*>(context)->RecordSchedulerEvent(event);
        });
}

bool AsyncStateGraph::TraceActive() const {
    return m_impl && m_impl->trace.load(std::memory_order_acquire) != nullptr;
}

AsyncStateGraphTraceReport AsyncStateGraph::StopTraceAndWriteReport(
    const std::filesystem::path& outputDirectory) {
    AsyncStateGraphTraceReport report;
    if (!m_impl) return report;
    if (auto* active = m_impl->trace.load(std::memory_order_acquire)) {
        m_impl->scheduler.RemoveTaskTraceSink(active);
    }
    auto* stopped = m_impl->trace.exchange(nullptr, std::memory_order_seq_cst);
    if (!stopped) return report;
    // Per-thread hazard slots protect a session between the raw-pointer load
    // and event append. They avoid the globally contended shared_ptr reference
    // count while still making trace stop a safe reclamation boundary.
    for (;;) {
        bool writerActive = false;
        for (auto* hazard = m_impl->traceHazards.load(std::memory_order_acquire);
            hazard; hazard = hazard->next) {
            if (hazard->pointer.load(std::memory_order_seq_cst) == stopped) {
                writerActive = true;
                break;
            }
        }
        if (!writerActive) break;
        std::this_thread::yield();
    }
    auto session = std::move(m_impl->traceOwner);
    if (!session || session.get() != stopped) return report;
    session->Record(AsyncStateGraphTraceEventID::TraceStopped, {});
    return session->Write(outputDirectory);
}

void AsyncStateGraph::TraceEvent(AsyncStateGraphTraceEventID event, ArtifactAddress address,
    std::uint64_t revision, std::uint64_t generation, AsyncStateGraphTracePayload payload,
    ArtifactAddress related, std::uint64_t relatedRevision) {
    if (auto session = m_impl->AcquireTrace()) {
        session->Record(event, address, revision, generation,
            ArtifactReadiness::Missing, 0, payload, related, relatedRevision);
    }
}

void AsyncStateGraph::WaitIdle() const {
    if (!m_impl) return;
    // Tests, shutdown, and explicit drains define idle as no CPU graph work;
    // submitted GPU versions remain valid schedulable artifacts. Temporarily
    // stop the recovery timer so an intentionally incomplete timeline does not
    // make WaitIdle wait for the GPU.
    m_impl->pauseGpuRecovery.store(true, std::memory_order_release);
	for (;;) {
		m_impl->scope.Wait();
		std::optional<std::chrono::steady_clock::duration> retryDelay;
		{
			auto lock = m_impl->LockMutex(GraphMutexPhase::WaitIdle);
			const auto now = std::chrono::steady_clock::now();
			for (const auto& [_, node] : m_impl->nodes) {
				if (!node.retryAt) continue;
				const auto remaining = *node.retryAt > now
					? *node.retryAt - now : std::chrono::steady_clock::duration::zero();
				if (!retryDelay || remaining < *retryDelay) retryDelay = remaining;
			}
		}
		if (!retryDelay) break;
		if (*retryDelay > std::chrono::steady_clock::duration::zero())
			std::this_thread::sleep_for(*retryDelay);
		m_impl->ScheduleDrain();
	}
    m_impl->pauseGpuRecovery.store(false, std::memory_order_release);
    bool resumeRecovery = false;
    {
        auto lock = m_impl->LockMutex(GraphMutexPhase::RecoveryResume);
        resumeRecovery = !m_impl->gpuRecovery.empty();
    }
    if (resumeRecovery) m_impl->ScheduleDrain();
}

void AsyncStateGraph::Shutdown() {
    if (!m_impl || m_impl->shuttingDown.exchange(true)) return;
    if (auto* active = m_impl->trace.load(std::memory_order_acquire)) {
        m_impl->scheduler.RemoveTaskTraceSink(active);
    }
    m_impl->drainPump.Stop();
    // Waiting is mandatory, but a task failure already captured at the
    // scheduler boundary must not unwind renderer teardown.
    try {
        m_impl->scope.CancelAndWait();
    } catch (const std::exception& exception) {
        spdlog::error("AsyncStateGraph shutdown observed task failure: {}", exception.what());
    } catch (...) {
        spdlog::error("AsyncStateGraph shutdown observed unknown task failure");
    }
    auto lock = m_impl->LockMutex(GraphMutexPhase::Shutdown);
    for (auto& [_, node] : m_impl->nodes) {
        if (node.waitingGpuSubmissions) (void)node.waitingGpuSubmissions->Cancel();
    }
    m_impl->pending.clear();
	Impl::Completion discardedCompletion;
	while (m_impl->completions.try_pop(discardedCompletion)) {
		if (discardedCompletion.result.gpuSubmissions)
			(void)discardedCompletion.result.gpuSubmissions->Cancel();
	}
	m_impl->completionCount.store(0, std::memory_order_release);
	Impl::GpuSignal discardedSignal;
    while (m_impl->gpuSignals.try_pop(discardedSignal)) {}
	Impl::StoredVersionKey discardedPublished;
	while (m_impl->publishedSignals.try_pop(discardedPublished)) {}
    m_impl->readyCallbacks.clear();
	m_impl->exactWaiters.clear();
	m_impl->exactWaiterCount = 0;
}

} // namespace br::render
