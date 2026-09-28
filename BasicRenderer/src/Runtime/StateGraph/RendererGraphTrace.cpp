#include "Runtime/StateGraph/RendererGraphAdapters.h"
#include "Runtime/StateGraph/GraphTrace.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <tbb/concurrent_queue.h>

namespace br::render {
namespace {
using namespace graph_detail;
using TaskTraceEvent = org::async::TaskTraceEvent;
using TaskTraceEventID = org::async::TaskTraceEventID;

std::string_view KindName(ArtifactKind kind) {
    static constexpr std::string_view names[]{ "Generic", "TextureBinding", "Material",
        "MaterialTable", "MaterialUsageBatch", "Mesh", "MeshTable", "GeometryBufferState",
        "DrawRecordPage", "ActiveDrawList", "ViewLifetime", "IndirectWorkload",
        "StaticTransaction", "StaticScenePage", "StaticScene", "TerrainState",
        "BufferVersion", "FrameManifest", "StaticGroup", "StaticTemplate",
        "StaticTemplateBatch", "StaticVisibility", "StaticAsset", "StaticMaterialVariant",
        "StaticShaderVariant", "StaticVariant", "TextureImageTable", "GrassCell",
        "GrassShard", "GrassScratch", "GrassScene", "GeometryResidency", "ViewFamily",
        "PoseState", "LightTable", "GeometryCoverageGate", "TextureDisplayGate",
        "CLodResidencyStorage", "CLodResidencyCapacityGate" };
    const auto index = static_cast<std::size_t>(kind);
    return index < std::size(names) ? names[index] : "Unknown";
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

class RendererGraphTraceSession final : public GraphTraceSession {
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
    explicit RendererGraphTraceSession(AsyncStateGraphTraceConfig config)
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
        summary << "\n## Graph drain phase timing\n\n"
            << "The graph has no control mutex: these are the phases of the single-consumer "
               "drain that owns graph state. Wait is always zero; hold is the time that "
               "consumer spent in the phase.\n\n"
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
        struct CacheEntry { const RendererGraphTraceSession* session = nullptr; std::uint64_t id = 0; TraceShard* shard = nullptr; };
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

std::shared_ptr<graph_detail::GraphTraceSession> MakeRendererGraphTrace(AsyncStateGraphTraceConfig config) {
    return std::make_shared<RendererGraphTraceSession>(config);
}

} // namespace br::render
