#include "Render/AsyncStateGraph.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <spdlog/spdlog.h>
#include <tbb/concurrent_queue.h>

#include <BasicTelemetry/Telemetry.h>

#include "Render/Runtime/StreamingUploadTypes.h"
#include "Managers/SerializedTaskPump.h"

namespace br::render {
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
        "StaticScene", "TerrainState", "BufferVersion", "FrameManifest" };
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
    std::uint64_t sequence = 0;
    std::int64_t timestampMicros = 0;
    std::uint64_t thread = 0;
    std::string event;
    ArtifactKey key{};
    std::uint64_t revision = 0;
    std::uint64_t generation = 0;
    ArtifactKey related{};
    std::uint64_t relatedRevision = 0;
    ArtifactReadiness readiness = ArtifactReadiness::Missing;
    std::int64_t durationMicros = 0;
    std::string detail;
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

class GraphTraceSession {
public:
    explicit GraphTraceSession(AsyncStateGraphTraceConfig config)
        : m_config(config), m_started(std::chrono::steady_clock::now()) {
        m_events.reserve((std::min<std::size_t>)(config.maximumEvents, 65536));
    }

    void Record(std::string event, ArtifactKey key = {}, std::uint64_t revision = 0,
        std::uint64_t generation = 0, ArtifactReadiness readiness = ArtifactReadiness::Missing,
        std::int64_t durationMicros = 0, std::string detail = {}, ArtifactKey related = {},
        std::uint64_t relatedRevision = 0) {
        GraphTraceEvent record;
        record.sequence = m_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        record.timestampMicros = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_started).count();
        record.thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
        record.event = std::move(event);
        record.key = key;
        record.revision = revision;
        record.generation = generation;
        record.related = related;
        record.relatedRevision = relatedRevision;
        record.readiness = readiness;
        record.durationMicros = durationMicros;
        record.detail = std::move(detail);
        std::lock_guard lock(m_mutex);
        if (m_events.size() >= m_config.maximumEvents) {
            ++m_dropped;
            return;
        }
        m_events.push_back(std::move(record));
    }

    AsyncStateGraphTraceReport Write(const std::filesystem::path& directory) {
        std::vector<GraphTraceEvent> events;
        std::uint64_t dropped = 0;
        {
            std::lock_guard lock(m_mutex);
            events = m_events;
            dropped = m_dropped;
        }
        std::ranges::sort(events, {}, &GraphTraceEvent::sequence);
        std::filesystem::create_directories(directory);
        AsyncStateGraphTraceReport report;
        report.eventsCsv = directory / "async_state_graph_events.csv";
        report.chromeTraceJson = directory / "async_state_graph_trace.json";
        report.summaryMarkdown = directory / "async_state_graph_summary.md";
        report.capturedEvents = events.size();
        report.droppedEvents = dropped;
        report.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_started);

        std::ofstream csv(report.eventsCsv, std::ios::trunc);
        csv << "sequence,timestamp_us,thread,event,kind,primary_id,variant_id,revision,generation,"
               "readiness,duration_us,related_kind,related_primary_id,related_variant_id,related_revision,detail\n";
        for (const auto& event : events) {
            csv << event.sequence << ',' << event.timestampMicros << ',' << event.thread << ','
                << CsvField(event.event) << ',' << static_cast<unsigned>(event.key.kind) << ','
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
            chrome << "{\"name\":\"" << JsonString(event.event) << "\",\"cat\":\"AsyncStateGraph\","
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
        std::map<TraceVersion, std::pair<ArtifactReadiness, std::int64_t>> lastState;
        std::vector<const GraphTraceEvent*> slowBuilds;
        for (const auto& event : events) {
            auto& aggregate = byEvent[event.event];
            ++aggregate.count;
            aggregate.total += event.durationMicros;
            aggregate.maximum = (std::max)(aggregate.maximum, event.durationMicros);
            if (event.event == "BuildCompleted") {
                auto& kind = buildsByKind[static_cast<unsigned>(event.key.kind)];
                ++kind.count;
                kind.total += event.durationMicros;
                kind.maximum = (std::max)(kind.maximum, event.durationMicros);
                slowBuilds.push_back(&event);
            }
            if (event.event == "StateChanged") {
                const TraceVersion version{ static_cast<unsigned>(event.key.kind),
                    event.key.primaryID, event.revision, event.generation };
                if (const auto previous = lastState.find(version); previous != lastState.end()) {
                    const auto duration = event.timestampMicros - previous->second.second;
                    auto& state = stateResidence[static_cast<unsigned>(previous->second.first)];
                    ++state.count;
                    state.total += duration;
                    state.maximum = (std::max)(state.maximum, duration);
                }
                lastState[version] = { event.readiness, event.timestampMicros };
            }
            if (event.event == "DependencyBlocked") {
                ++blockerEdges[{ static_cast<unsigned>(event.key.kind),
                    static_cast<unsigned>(event.related.kind) }];
            }
        }
        std::ranges::sort(slowBuilds, std::greater{}, [](const GraphTraceEvent* event) {
            return event->durationMicros;
        });
        std::ofstream summary(report.summaryMarkdown, std::ios::trunc);
        summary << "# Async State Graph Trace\n\n"
            << "- Elapsed: " << report.elapsed.count() << " us\n"
            << "- Captured events: " << report.capturedEvents << "\n"
            << "- Dropped events: " << report.droppedEvents << "\n\n"
            << "## Event totals\n\n| Event | Count | Total duration (us) | Maximum (us) |\n"
            << "|---|---:|---:|---:|\n";
        for (const auto& [name, aggregate] : byEvent) {
            summary << "| " << name << " | " << aggregate.count << " | " << aggregate.total
                << " | " << aggregate.maximum << " |\n";
        }
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
            << "| Readiness | Completed intervals | Total (us) | Maximum (us) |\n"
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
    AsyncStateGraphTraceConfig m_config;
    std::chrono::steady_clock::time_point m_started;
    std::atomic_uint64_t m_sequence{0};
    std::mutex m_mutex;
    std::vector<GraphTraceEvent> m_events;
    std::uint64_t m_dropped = 0;
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
        ArtifactLease lease;
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
        std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
        std::shared_ptr<const GpuSubmissionSet> waitingGpuSubmissions;
        std::optional<std::chrono::steady_clock::time_point> retryAt;
        std::string error;
        std::chrono::steady_clock::time_point stateSince = std::chrono::steady_clock::now();
        bool buildInFlight = false;
        bool buildAttempted = false;
        bool desired = true;
        bool terminalFailure = false;
        bool published = false;
        bool latestSuccessorNeeded = false;
        std::chrono::steady_clock::time_point queuedAt{};
        std::chrono::steady_clock::time_point buildStartedAt{};
        std::chrono::steady_clock::time_point uploadSubmittedAt{};
        std::deque<RequestedVersion> successors;
    };

    struct Completion {
        ArtifactKey key;
        std::uint64_t revision = 0;
        std::uint64_t generation = 0;
        std::vector<ArtifactSnapshot> dependencies;
        ArtifactBuildResult result;
    };

    TaskSchedulerManager& scheduler;
    TaskScope scope;
    mutable std::mutex mutex;
    std::unordered_map<ArtifactKey, Node, ArtifactKey::Hasher> nodes;
    // Completed versions are immutable. The mutable address slot above is only
    // the desired/build cursor; ExactSnapshot never resolves through that cursor.
    std::unordered_map<StoredVersionKey, ArtifactSnapshot, StoredVersionKey::Hasher> versions;
    std::unordered_map<VersionKey, std::uint64_t, VersionKey::Hasher> versionGenerations;
    std::unordered_map<VersionKey, VersionSignature, VersionKey::Hasher> versionSignatures;
    mutable std::unordered_map<StoredVersionKey, std::weak_ptr<const void>, StoredVersionKey::Hasher> versionLeases;
    std::uint64_t nextVersionGeneration = 0;
    std::uint64_t reclaimedVersions = 0;
    std::unordered_map<ArtifactKey, std::unordered_set<ArtifactKey, ArtifactKey::Hasher>, ArtifactKey::Hasher> waiters;
    std::unordered_map<ArtifactKind, ArtifactProducerRegistration> producers;
    std::deque<ArtifactKey> pending;
    std::deque<Completion> completions;
    tbb::concurrent_queue<ArtifactKey> gpuSignals;
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
	std::uint64_t nextExactWaiter = 0;
    AsyncStateGraphStats stats;
    br::SerializedTaskPump drainPump;
    std::atomic_bool delayedDrainScheduled{ false };
    std::atomic_bool shuttingDown{ false };
    std::atomic_bool pauseGpuRecovery{ false };
    std::atomic<std::shared_ptr<GraphTraceSession>> trace;

    Impl(TaskSchedulerManager& schedulerIn, std::string_view name)
        : scheduler(schedulerIn), scope(schedulerIn.CreateScope(name)) {}

    void ConfigureDrainPump() {
        auto weak = weak_from_this();
        drainPump.Configure(
            [weak](br::SerializedTaskPump::Task task) {
                auto self = weak.lock();
                return self && self->scheduler.Submit(
                    self->scope, TaskLane::Streaming, TaskDomain::RendererState,
                    "AsyncStateGraph::Drain",
                    [task = std::move(task)](const TaskContext& context) mutable {
                        if (!context.StopRequested()) task();
                    });
            },
            [weak] {
                if (auto self = weak.lock()) self->Drain();
            });
    }

    void SetState(Node& node, ArtifactReadiness state) {
        const auto previous = node.state;
        node.state = state;
        node.stateSince = std::chrono::steady_clock::now();
        if (auto session = trace.load(std::memory_order_acquire)) {
            session->Record("StateChanged", node.key, node.desiredRevision,
                node.versionGeneration, state, 0,
                std::format("{}->{}", static_cast<unsigned>(previous), static_cast<unsigned>(state)));
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
            [weak](const void* value) {
                delete static_cast<const std::uint8_t*>(value);
                if (auto graph = weak.lock()) graph->ScheduleDrain();
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
        versions.insert_or_assign(StoredVersionKey{ node.key, node.producedRevision,
            node.versionGeneration }, std::move(snapshot));
    }

    bool RecipeReferences(const StoredVersionKey& version,
        const std::vector<ArtifactRequirement>& requirements) const {
        return std::ranges::any_of(requirements, [&](const ArtifactRequirement& requirement) {
            return requirement.key == version.address &&
                requirement.minimumRevision == version.revision &&
                requirement.invalidation != DependencyInvalidationPolicy::Latest &&
                (requirement.requiredGeneration == 0 ||
                 requirement.requiredGeneration == version.generation);
        });
    }

    void ReclaimUnreferencedVersions() {
        // Requirements are deliberately non-owning values, but a live graph
        // recipe is an ownership root. Build one deduplicated pin index across
        // every active recipe before examining the archive. The old code only
        // inspected the node at version.address, so an Exact edge from a
        // different consumer did not retain its dependency. Permanent
        // signature leases happened to hide that error until those leases were
        // correctly removed.
        std::unordered_set<StoredVersionKey, StoredVersionKey::Hasher> dependencyPins;
        const auto collectPins = [&](const std::vector<ArtifactRequirement>& requirements) {
            for (const auto& requirement : requirements) {
                if (requirement.minimumRevision == 0 ||
                    requirement.invalidation == DependencyInvalidationPolicy::Latest) continue;
                if (requirement.requiredGeneration != 0) dependencyPins.insert({
                    requirement.key, requirement.minimumRevision, requirement.requiredGeneration });
            }
        };
        for (const auto& [_, node] : nodes) {
            collectPins(node.requirements);
            collectPins(node.requestedRequirements);
            for (const auto& successor : node.successors) collectPins(successor.requirements);
        }
        for (auto version = versions.begin(); version != versions.end();) {
            const auto lease = versionLeases.find(version->first);
            bool referenced = dependencyPins.contains(version->first) ||
                (lease != versionLeases.end() && !lease->second.expired());
            if (const auto current = nodes.find(version->first.address); current != nodes.end()) {
                const auto& node = current->second;
                referenced = referenced ||
                    (node.desiredRevision == version->first.revision &&
                     node.versionGeneration == version->first.generation) ||
                    (node.producedRevision == version->first.revision &&
                     node.versionGeneration == version->first.generation) ||
                    std::ranges::any_of(node.successors, [&](const RequestedVersion& successor) {
                        return successor.revision == version->first.revision &&
                            successor.generation == version->first.generation;
                    });
            }
            if (referenced) {
                ++version;
                continue;
            }
            const auto reclaimed = version->first;
            version = versions.erase(version);
            ++reclaimedVersions;
            basic_telemetry::AddCounter("SARP.AsyncStateGraph.ReclaimedVersions");
            if (auto session = trace.load(std::memory_order_acquire);
                session && session->Config().includeRetentionEvents) {
                session->Record("VersionReclaimed", reclaimed.address, reclaimed.revision,
                    reclaimed.generation);
            }
        }
        std::erase_if(versionLeases, [&](const auto& entry) {
            return entry.second.expired() && !versions.contains(entry.first);
        });
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
                for (const auto& [key, snapshot] : versions) {
                    if (key.address == requirement.key && key.revision == requirement.minimumRevision &&
                        (!newest || snapshot.generation > newest->generation)) newest = &snapshot;
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
        const auto current = nodes.find(requirement.key);
        if (current == nodes.end() ||
            current->second.producedRevision < requirement.minimumRevision) return nullptr;
        static thread_local ArtifactSnapshot selected;
        selected = MakeSnapshot(current->second);
        return &selected;
    }

    void RemoveWaiterEdges(const Node& node) {
        const auto remove = [&](const ArtifactRequirement& requirement) {
            if (auto found = waiters.find(requirement.key); found != waiters.end()) {
                found->second.erase(node.key);
                if (found->second.empty()) waiters.erase(found);
            }
        };
        for (const auto& requirement : node.requirements) remove(requirement);
        for (const auto& requirement : node.requestedRequirements) remove(requirement);
    }

    void InstallWaiterEdges(const Node& node) {
        for (const auto& requirement : node.requirements) waiters[requirement.key].insert(node.key);
        for (const auto& requirement : node.requestedRequirements)
            waiters[requirement.key].insert(node.key);
    }

    bool FindPath(const ArtifactKey& from, const ArtifactKey& target,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited,
        std::vector<ArtifactKey>& path) const {
        if (!visited.insert(from).second) return false;
        path.push_back(from);
        if (from == target) return true;
        const auto found = nodes.find(from);
        if (found != nodes.end()) {
            for (const auto& requirement : found->second.requirements) {
                if (requirement.policy == DependencyPolicy::Optional ||
                    requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
                if (FindPath(requirement.key, target, visited, path)) return true;
            }
        }
        path.pop_back();
        return false;
    }

    std::vector<ArtifactKey> DetectCycle(const Node& node) const {
        for (const auto& requirement : node.requirements) {
            if (requirement.policy == DependencyPolicy::Optional ||
                requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
            std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
            std::vector<ArtifactKey> path;
            if (!FindPath(requirement.key, node.key, visited, path)) continue;
            path.insert(path.begin(), node.key);
            return path;
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
            for (const auto& [versionKey, snapshot] : versions) {
                if (versionKey.address != requirement.key ||
                    !Satisfies(snapshot.readiness, requirement.requiredReadiness) ||
                    (selected && snapshot.revision <= selected->revision)) continue;
                selected = &snapshot;
            }
            if (!selected) continue;
            requirement.minimumRevision = selected->revision;
            requirement.requiredGeneration = selected->generation;
        }
    }

    bool DependenciesSatisfied(const Node& node) const {
        std::unordered_map<std::uint32_t, bool> anyGroups;
        std::unordered_map<std::uint32_t, bool> fallbackGroups;
        for (const auto& requirement : node.requirements) {
            const bool satisfied = RequirementSatisfied(requirement);
            if (requirement.invalidation == DependencyInvalidationPolicy::LifetimeHold) continue;
            switch (requirement.policy) {
            case DependencyPolicy::Optional: break;
            case DependencyPolicy::AnyOf:
                anyGroups[requirement.alternativeGroup] |= satisfied;
                break;
            case DependencyPolicy::AllOf:
                if (!satisfied) return false;
                break;
            case DependencyPolicy::FallbackAllowed:
                fallbackGroups[requirement.alternativeGroup] |= satisfied;
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
        LatchReadyGates(node);
        if (!DependenciesSatisfied(node)) {
            SetState(node, ArtifactReadiness::Blocked);
            if (auto session = trace.load(std::memory_order_acquire);
                session && session->Config().includeDependencyEvents) {
                for (const auto& requirement : node.requirements) {
                    if (!RequirementSatisfied(requirement)) {
                        session->Record("DependencyBlocked", node.key, node.desiredRevision,
                            node.versionGeneration, node.state, 0,
                            std::format("policy={} milestone={} generation={}",
                                static_cast<unsigned>(requirement.invalidation),
                                static_cast<unsigned>(requirement.requiredReadiness),
                                requirement.requiredGeneration),
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
        node.successors.pop_front();
        node.desiredRevision = successor.revision;
        node.versionGeneration = successor.generation;
        node.requestFingerprint = successor.fingerprint;
        node.lease = std::move(successor.lease);
        node.requirements = std::move(successor.requirements);
        node.requestedRequirements = node.requirements;
        node.input = std::move(successor.input);
        node.checkpoint = {};
        node.payload = {};
        node.resolvedDependencies.clear();
        node.gpuSubmissions.reset();
        node.waitingGpuSubmissions.reset();
        node.producedRevision = 0;
        node.published = false;
        node.terminalFailure = false;
        node.latestSuccessorNeeded = false;
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
        for (const auto& requirement : node.requestedRequirements) {
            if (requirement.invalidation != DependencyInvalidationPolicy::Latest) continue;
            const auto* selected = SelectSnapshot(requirement);
            if (!selected) continue;
            HashRequestValue(fingerprint, selected->revision);
            HashRequestValue(fingerprint, selected->generation);
        }
        if (fingerprint == 0) fingerprint = 1;
        node.successors.push_back({ revision, generation, fingerprint, node.input,
            node.requestedRequirements, AcquireVersionLease({ node.key, revision, generation }) });
        node.latestSuccessorNeeded = false;
        ++stats.requests;
        return PromoteSuccessor(node);
    }

    void WakeWaiters(const ArtifactKey& key) {
        const auto found = waiters.find(key);
        if (found == waiters.end()) return;
        const auto keys = found->second;
        for (const auto& waiter : keys) {
            const auto nodeFound = nodes.find(waiter);
            if (nodeFound == nodes.end()) continue;
            auto& node = nodeFound->second;

            bool keyIsSelected = false;
            DependencyInvalidationPolicy invalidation = DependencyInvalidationPolicy::Latest;
            for (const auto& requirement : node.requestedRequirements) {
                if (requirement.key != key) continue;
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
                break;
            }

            const auto current = nodes.find(key);
            const bool exactSelectionAlreadyUsed = current != nodes.end() &&
                std::ranges::any_of(node.resolvedDependencies,
                    [&](const ArtifactSnapshot& dependency) {
                        return dependency.key == key &&
                            dependency.revision == current->second.producedRevision &&
                            dependency.generation == current->second.versionGeneration;
                    });
            const bool completedConsumer =
                node.state == ArtifactReadiness::GpuReady ||
                node.state == ArtifactReadiness::Published ||
                node.state == ArtifactReadiness::UploadSubmitted;
            if (completedConsumer && keyIsSelected && !exactSelectionAlreadyUsed &&
                invalidation == DependencyInvalidationPolicy::Latest) {
                QueueLatestSuccessor(node);
                continue;
            }
            if (node.buildAttempted && keyIsSelected && !exactSelectionAlreadyUsed &&
                invalidation == DependencyInvalidationPolicy::Latest) {
                node.latestSuccessorNeeded = true;
            }
            QueueNode(node);
        }
    }

    void SupersedeDependents(const ArtifactKey& key,
        std::unordered_set<ArtifactKey, ArtifactKey::Hasher>& visited) {
        if (!visited.insert(key).second) return;
        const auto found = waiters.find(key);
        if (found == waiters.end()) return;
        const auto dependentKeys = found->second;
        for (const auto& dependentKey : dependentKeys) {
            const auto dependent = nodes.find(dependentKey);
            if (dependent == nodes.end()) continue;
            auto& node = dependent->second;
            const bool rebuildsForLatest = std::ranges::any_of(node.requestedRequirements,
                [&](const ArtifactRequirement& requirement) {
                    return requirement.key == key &&
                        requirement.invalidation == DependencyInvalidationPolicy::Latest;
                });
            if (!rebuildsForLatest) continue;
            // A request does not invalidate the selected closure of a version
            // already being built. The ready milestone will wake this edge and
            // produce a new immutable successor.
            continue;
        }
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
            scope, delay, TaskLane::Streaming, TaskDomain::RendererState,
            "AsyncStateGraph::DelayedDrain",
            [weak](const TaskContext& context) {
                if (auto self = weak.lock()) {
                    self->delayedDrainScheduled.store(false, std::memory_order_release);
                    if (!context.StopRequested()) (void)self->drainPump.Notify();
                }
            });
        if (!submitted) delayedDrainScheduled.store(false, std::memory_order_release);
    }

    void SubmitBuild(const ArtifactProducerRegistration& registration, ArtifactBuildContext context) {
        auto weak = weak_from_this();
        const auto key = context.key;
        const auto revision = context.revision;
        const auto generation = context.generation;
        auto dependencyStamp = context.dependencies;
        if (auto session = trace.load(std::memory_order_acquire)) {
            session->Record("BuildSubmitted", key, revision, generation,
                ArtifactReadiness::Preparing, 0, registration.taskName);
        }
        const bool submitted = scheduler.Submit(scope, registration.lane, registration.domain,
            registration.taskName.empty() ? "AsyncStateGraph::Build" : registration.taskName,
            [weak, registration, context = std::move(context), key, revision, generation,
                dependencyStamp = std::move(dependencyStamp)](const TaskContext& cancellation) mutable {
                auto self = weak.lock();
                if (!self) return;
                const auto producerStarted = std::chrono::steady_clock::now();
                if (auto session = self->trace.load(std::memory_order_acquire)) {
                    session->Record("BuildStarted", key, revision, generation,
                        ArtifactReadiness::Preparing, 0, registration.taskName);
                }
                context.stopRequested = [cancellation] { return cancellation.StopRequested(); };
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
                if (auto session = self->trace.load(std::memory_order_acquire)) {
                    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - producerStarted).count();
                    session->Record("BuildCompleted", key, revision, generation,
                        ArtifactReadiness::Preparing, duration,
                        std::format("outcome={} task={}", static_cast<unsigned>(result.outcome),
                            registration.taskName));
                }
                {
                    std::lock_guard lock(self->mutex);
                    self->completions.push_back({ key, revision, generation,
                        std::move(dependencyStamp), std::move(result) });
                }
                self->ScheduleDrain();
            });
        if (!submitted) {
            if (auto session = trace.load(std::memory_order_acquire)) {
                session->Record("BuildRejected", key, revision, generation,
                    ArtifactReadiness::Failed);
            }
            {
                std::lock_guard lock(mutex);
                completions.push_back({ key, revision, generation, std::move(dependencyStamp),
                    ArtifactBuildResult::Failure("scheduler rejected producer") });
            }
            ScheduleDrain();
        }
    }

    void ApplyCompletion(Completion completion, std::vector<ArtifactSnapshot>& ready,
        std::vector<std::pair<std::function<void(const ArtifactSnapshot&)>, ArtifactSnapshot>>& accepted) {
        const auto found = nodes.find(completion.key);
        if (found == nodes.end()) return;
        auto& node = found->second;
        if (node.generation != completion.generation || !DependenciesStillMatch(completion)) {
            ++stats.staleCompletions;
            basic_telemetry::AddCounter("SARP.AsyncStateGraph.StaleCompletions");
            node.buildInFlight = false;
            if (auto session = trace.load(std::memory_order_acquire)) {
                session->Record("CompletionStale", completion.key, completion.revision,
                    completion.generation, node.state);
            }
            QueueNode(node);
            return;
        }
        node.buildInFlight = false;
        ++stats.buildsCompleted;
        if (node.buildStartedAt != std::chrono::steady_clock::time_point{}) {
            const auto buildDuration = std::chrono::steady_clock::now() - node.buildStartedAt;
            stats.buildMicros += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(buildDuration).count());
            basic_telemetry::Record("SARP.AsyncStateGraph.BuildLatencyNs",
                static_cast<std::uint64_t>(
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
        case ArtifactBuildResult::Outcome::Ready:
            node.terminalFailure = false;
            node.payload = std::move(result.payload);
            node.resolvedDependencies = completion.dependencies;
            node.checkpoint = {};
            node.retryAt.reset();
            node.producedRevision = completion.revision;
            node.published = false;
            node.gpuSubmissions = std::move(result.gpuSubmissions);
            if (result.onAccepted) accepted.emplace_back(
                std::move(result.onAccepted), MakeSnapshot(node));
            if (node.gpuSubmissions && !node.gpuSubmissions->Submitted()) {
                node.waitingGpuSubmissions = node.gpuSubmissions;
                SetState(node, ArtifactReadiness::CpuReady);
                ++stats.gpuWaiting;
                gpuRecovery.push_back(node.key);
                if (node.waitingGpuSubmissions->subscribe) {
                    auto weak = weak_from_this();
                    const auto key = node.key;
                    node.waitingGpuSubmissions->subscribe([weak, key] {
                        if (auto self = weak.lock()) {
                            {
                                self->gpuSignals.push(key);
                            }
                            self->ScheduleDrain();
                        }
                    });
                }
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
                WakeWaiters(node.key);
            } else if (node.gpuSubmissions && !node.gpuSubmissions->Complete()) {
                node.waitingGpuSubmissions = node.gpuSubmissions;
                SetState(node, ArtifactReadiness::UploadSubmitted);
                node.uploadSubmittedAt = std::chrono::steady_clock::now();
                ++stats.gpuWaiting;
                gpuRecovery.push_back(node.key);
                if (node.waitingGpuSubmissions->subscribe) {
                    auto weak = weak_from_this();
                    const auto key = node.key;
                    node.waitingGpuSubmissions->subscribe([weak, key] {
                        if (auto self = weak.lock()) {
                            self->gpuSignals.push(key);
                            self->ScheduleDrain();
                        }
                    });
                }
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
                WakeWaiters(node.key);
                if (node.latestSuccessorNeeded) QueueLatestSuccessor(node);
                else PromoteSuccessor(node);
            } else {
                node.waitingGpuSubmissions.reset();
                SetState(node, node.published ? ArtifactReadiness::Published
                                             : ArtifactReadiness::GpuReady);
                ready.push_back(MakeSnapshot(node));
                StoreVersion(node);
                WakeWaiters(node.key);
                if (node.latestSuccessorNeeded) QueueLatestSuccessor(node);
                else PromoteSuccessor(node);
            }
            break;
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
            SetState(node, ArtifactReadiness::Blocked);
            ++stats.retries;
            break;
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
        };
        constexpr std::size_t maxTransitions = 128;
        constexpr auto maxDuration = std::chrono::milliseconds(2);
        const auto started = std::chrono::steady_clock::now();
        if (auto session = trace.load(std::memory_order_acquire)) {
            session->Record("DrainStarted");
        }
        std::size_t transitions = 0;
        std::vector<PendingGpuSignal> signalledGpu;
        std::vector<std::pair<std::function<void(const ArtifactSnapshot&)>, ArtifactSnapshot>> accepted;
        {
            std::lock_guard lock(mutex);
            ArtifactKey key;
            while (gpuSignals.try_pop(key) && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                const auto found = nodes.find(key);
                if (found != nodes.end() &&
                    (found->second.state == ArtifactReadiness::CpuReady ||
                     found->second.state == ArtifactReadiness::UploadSubmitted) &&
                    found->second.waitingGpuSubmissions) {
                    signalledGpu.push_back({ key, found->second.generation,
                        found->second.waitingGpuSubmissions });
                }
            }
            // Submission notifications are wakeups, not ownership-transfer events:
            // a ticket commonly notifies while its timeline is still incomplete.
            // Keep every submitted node in this rotating level-triggered set until
            // Complete() observes the timeline. A finite retry budget strands nodes
            // whenever GPU latency exceeds that budget and recreates a missed-callback
            // correctness gate in every consumer.
            constexpr std::size_t maxGpuRecoveryChecks = 32;
            const auto recoveryChecks = (std::min)(maxGpuRecoveryChecks, gpuRecovery.size());
            for (std::size_t index = 0; index < recoveryChecks; ++index) {
                const auto recoveryKey = gpuRecovery.front();
                gpuRecovery.pop_front();
                const auto found = nodes.find(recoveryKey);
                if (found == nodes.end() ||
                    (found->second.state != ArtifactReadiness::CpuReady &&
                     found->second.state != ArtifactReadiness::UploadSubmitted) ||
                    !found->second.waitingGpuSubmissions) continue;
                signalledGpu.push_back({ recoveryKey, found->second.generation,
                    found->second.waitingGpuSubmissions });
                gpuRecovery.push_back(recoveryKey);
            }
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
        std::vector<std::pair<ArtifactProducerRegistration, ArtifactBuildContext>> builds;
        std::vector<ArtifactSnapshot> ready;
        std::vector<std::function<void(const ArtifactSnapshot&)>> callbacks;
		std::vector<std::pair<ExactWaiter, ArtifactSnapshot>> exactDispatches;
        std::optional<std::chrono::steady_clock::duration> retryDelay;
        bool hasImmediateWork = false;
        {
            std::lock_guard lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            for (const auto& evaluation : evaluatedGpu) {
                const auto& signal = evaluation.signal;
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
                    basic_telemetry::Record("SARP.AsyncStateGraph.GpuLatencyNs",
                        static_cast<std::uint64_t>(
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
            for (auto& [_, node] : nodes) {
                if (!node.retryAt) continue;
                if (*node.retryAt <= now) {
                    node.retryAt.reset();
                    QueueNode(node);
                } else {
                    const auto remaining = *node.retryAt - now;
                    retryDelay = retryDelay ? (std::min)(*retryDelay, remaining) : remaining;
                }
            }
            while (!completions.empty() && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
                auto completion = std::move(completions.front());
                completions.pop_front();
                const auto completedKey = completion.key;
                ApplyCompletion(std::move(completion), ready, accepted);
                const auto completed = nodes.find(completedKey);
                if (completed != nodes.end() && !completed->second.desired &&
                    !completed->second.buildInFlight) {
                    const auto dependents = waiters.find(completedKey);
                    if (dependents == waiters.end() || dependents->second.empty()) {
                        nodes.erase(completed);
                    }
                }
            }
            while (!pending.empty() && transitions++ < maxTransitions &&
                std::chrono::steady_clock::now() - started < maxDuration) {
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
                    basic_telemetry::Record("SARP.AsyncStateGraph.QueueLatencyNs",
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(queueWait).count()));
                }
                SetState(node, ArtifactReadiness::Preparing);
                ++stats.buildsStarted;
                ArtifactBuildContext context{ node.key, node.desiredRevision, node.generation,
                    DependencySnapshots(node), node.input, node.checkpoint, {} };
                builds.emplace_back(producer->second, std::move(context));
            }
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
					registered[index] = std::move(registered.back());
					registered.pop_back();
				}
				if (registered.empty()) exactWaiters.erase(found);
			}
            callbacks.reserve(readyCallbacks.size());
            for (const auto& [_, callback] : readyCallbacks) callbacks.push_back(callback);
            hasImmediateWork = !pending.empty() || !completions.empty() || !gpuSignals.empty();
            const auto afterWork = std::chrono::steady_clock::now();
            if (!gpuRecovery.empty() && !pauseGpuRecovery.load(std::memory_order_acquire)) {
                const auto recoveryDelay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::milliseconds(8));
                retryDelay = retryDelay ? (std::min)(*retryDelay, recoveryDelay) : recoveryDelay;
            }
            for (const auto& [_, node] : nodes) {
                if (!node.retryAt) continue;
                const auto remaining = *node.retryAt > afterWork
                    ? *node.retryAt - afterWork
                    : std::chrono::steady_clock::duration::zero();
                retryDelay = retryDelay ? (std::min)(*retryDelay, remaining) : remaining;
            }
            ReclaimUnreferencedVersions();
            if (basic_telemetry::Enabled()) {
                std::array<std::uint64_t, static_cast<std::size_t>(ArtifactKind::FrameManifest) + 1u>
                    kindCounts{};
                std::array<std::uint64_t, static_cast<std::size_t>(ArtifactKind::FrameManifest) + 1u>
                    archivedKindCounts{};
                std::uint64_t maxBlockerAgeMicros = 0;
                std::uint64_t maxFanout = 0;
                for (const auto& [nodeKey, node] : nodes) {
                    const auto kindIndex = static_cast<std::size_t>(nodeKey.kind);
                    if (kindIndex < kindCounts.size()) ++kindCounts[kindIndex];
                    if (node.state == ArtifactReadiness::Blocked) {
                        maxBlockerAgeMicros = (std::max)(maxBlockerAgeMicros,
                            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                afterWork - node.stateSince).count()));
                    }
                    if (const auto fanout = waiters.find(nodeKey); fanout != waiters.end()) {
                        maxFanout = (std::max)(maxFanout,
                            static_cast<std::uint64_t>(fanout->second.size()));
                    }
                }
                std::unordered_set<StoredVersionKey, StoredVersionKey::Hasher> dependencyPins;
                const auto collectPins = [&](const std::vector<ArtifactRequirement>& requirements) {
                    for (const auto& requirement : requirements) {
                        if (requirement.minimumRevision == 0 ||
                            requirement.invalidation == DependencyInvalidationPolicy::Latest) continue;
                        if (requirement.requiredGeneration != 0) dependencyPins.insert({
                            requirement.key, requirement.minimumRevision,
                            requirement.requiredGeneration });
                    }
                };
                for (const auto& [_, node] : nodes) {
                    collectPins(node.requirements);
                    collectPins(node.requestedRequirements);
                    for (const auto& successor : node.successors) collectPins(successor.requirements);
                }
                for (const auto& [versionKey, _] : versions) {
                    const auto kindIndex = static_cast<std::size_t>(versionKey.address.kind);
                    if (kindIndex < archivedKindCounts.size()) ++archivedKindCounts[kindIndex];
                }
                std::uint64_t externallyLeasedVersions = 0;
                std::uint64_t desiredVersions = 0;
                std::uint64_t recipePinnedVersions = 0;
                std::uint64_t unclassifiedRetainedVersions = 0;
                for (const auto& [versionKey, _] : versions) {
                    const auto lease = versionLeases.find(versionKey);
                    const bool external = lease != versionLeases.end() && !lease->second.expired();
                    bool desired = false;
                    bool recipe = false;
                    if (const auto current = nodes.find(versionKey.address); current != nodes.end()) {
                        const auto& node = current->second;
                        desired = (node.desiredRevision == versionKey.revision &&
                            node.versionGeneration == versionKey.generation) ||
                            (node.producedRevision == versionKey.revision &&
                             node.versionGeneration == versionKey.generation);
                    }
                    recipe = dependencyPins.contains(versionKey);
                    externallyLeasedVersions += external;
                    desiredVersions += desired;
                    recipePinnedVersions += recipe;
                    unclassifiedRetainedVersions += !external && !desired && !recipe;
                }
                stats.externallyLeasedVersions = externallyLeasedVersions;
                stats.desiredVersions = desiredVersions;
                stats.recipePinnedVersions = recipePinnedVersions;
                stats.unclassifiedRetainedVersions = unclassifiedRetainedVersions;
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Nodes",
                    static_cast<std::int64_t>(nodes.size()));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.ArchivedVersions",
                    static_cast<std::int64_t>(versions.size()));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.ReclaimedVersionsTotal",
                    static_cast<std::int64_t>(reclaimedVersions));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Retention.ExternalLeaseVersions",
                    static_cast<std::int64_t>(externallyLeasedVersions));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Retention.DesiredVersions",
                    static_cast<std::int64_t>(desiredVersions));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Retention.RecipePinnedVersions",
                    static_cast<std::int64_t>(recipePinnedVersions));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Retention.UnclassifiedVersions",
                    static_cast<std::int64_t>(unclassifiedRetainedVersions));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Retention.SignatureOwnedLeases", 0);
				std::uint64_t exactWaiterCount = 0;
				for (const auto& [_, registrations] : exactWaiters) {
					exactWaiterCount += registrations.size();
				}
				stats.exactWaiters = exactWaiterCount;
				basic_telemetry::SetGauge("SARP.AsyncStateGraph.ExactWaiters",
					static_cast<std::int64_t>(exactWaiterCount));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Pending",
                    static_cast<std::int64_t>(pending.size()));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.GpuWaiting",
                    static_cast<std::int64_t>(stats.gpuWaiting));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.BlockerAgeMicros",
                    static_cast<std::int64_t>(maxBlockerAgeMicros));
                basic_telemetry::SetGauge("SARP.AsyncStateGraph.Fanout",
                    static_cast<std::int64_t>(maxFanout));
                for (std::size_t index = 0; index < kindCounts.size(); ++index) {
                    basic_telemetry::SetGauge(
                        std::format("SARP.AsyncStateGraph.Kind.{}.Nodes", index),
                        static_cast<std::int64_t>(kindCounts[index]));
                    basic_telemetry::SetGauge(
                        std::format("SARP.AsyncStateGraph.Kind.{}.ArchivedVersions", index),
                        static_cast<std::int64_t>(archivedKindCounts[index]));
                }
            }
        }
        // Immediate follow-up work is a level-triggered mailbox notification.
        // If it arrives while this drain owns the consumer, the same worker
        // loops; otherwise the notifying producer schedules the successor.
        hasImmediateWork = hasImmediateWork || !gpuSignals.empty();
        for (auto& [registration, context] : builds) SubmitBuild(registration, std::move(context));
		for (auto& [action, snapshot] : accepted) if (action) action(snapshot);
		for (auto& [waiter, snapshot] : exactDispatches) {
			if (!waiter.continuation) continue;
			if (auto session = trace.load(std::memory_order_acquire)) {
				session->Record("ExactWaitSatisfied", waiter.version.address,
					waiter.version.revision, waiter.version.generation, snapshot.readiness,
					0, std::format("subscription={}", waiter.subscription));
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
        if (auto session = trace.load(std::memory_order_acquire)) {
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            session->Record("DrainCompleted", {}, 0, 0, ArtifactReadiness::Missing,
                duration, std::format("transitions={} builds={} ready={} gpuSignals={}",
                    transitions, builds.size(), ready.size(), signalledGpu.size()));
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

void AsyncStateGraph::RegisterProducer(ArtifactKind kind, ArtifactProducerRegistration registration) {
    std::lock_guard lock(m_impl->mutex);
    m_impl->producers[kind] = std::move(registration);
}

ArtifactRequestResult AsyncStateGraph::Request(ArtifactKey key, std::uint64_t desiredRevision,
    std::vector<ArtifactRequirement> requirements, ArtifactPayload input,
    std::uint64_t requestFingerprint) {
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("RequestReceived", key, desiredRevision, 0,
            ArtifactReadiness::Missing, 0,
            std::format("dependencies={} fingerprint={}", requirements.size(), requestFingerprint));
    }
    if (m_impl->shuttingDown.load(std::memory_order_acquire)) {
        return { ArtifactRequestStatus::ShuttingDown, 0 };
    }
    ArtifactRequestResult result;
    {
        std::lock_guard lock(m_impl->mutex);
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
        const Impl::VersionKey requestedVersion{ key, desiredRevision };
        auto versionGeneration = m_impl->versionGenerations.find(requestedVersion);
        if (versionGeneration == m_impl->versionGenerations.end()) {
            versionGeneration = m_impl->versionGenerations.emplace(
                requestedVersion, ++m_impl->nextVersionGeneration).first;
        }
        auto& node = m_impl->nodes[key];
        node.key = key;
        node.desired = true;
        const auto knownSignature = m_impl->versionSignatures.find(requestedVersion);
        if (knownSignature != m_impl->versionSignatures.end()) {
            if (knownSignature->second.fingerprint != requestFingerprint ||
                knownSignature->second.inputType != input.Type() ||
                knownSignature->second.requirements != requirements) {
                if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
                    session->Record("RequestConflict", key, desiredRevision,
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
                if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
                    session->Record("RequestAlreadyDesired", key, desiredRevision,
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
                requestFingerprint, input.Type(), requirements });
        }
        node.latestRequestedRevision = desiredRevision;
        ++m_impl->stats.requests;

        const bool activeVersionExists = node.desiredRevision != 0;
        const bool activeVersionFinished = activeVersionExists &&
            node.producedRevision == node.desiredRevision &&
            (node.state == ArtifactReadiness::GpuReady ||
             node.state == ArtifactReadiness::Published);
        if (activeVersionExists && !activeVersionFinished) {
            node.successors.push_back({ desiredRevision, versionGeneration->second, requestFingerprint,
                std::move(input), std::move(requirements), versionLease });
            if (node.state == ArtifactReadiness::UploadSubmitted && !node.buildInFlight) {
                m_impl->PromoteSuccessor(node);
            }
            std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
            m_impl->SupersedeDependents(key, visited);
            result = { ArtifactRequestStatus::Accepted, node.generation,
                { key, desiredRevision, versionGeneration->second }, versionLease };
            if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
                session->Record("SuccessorQueued", key, desiredRevision,
                    versionGeneration->second, node.state);
            }
            // The active immutable version retains its own input and closure.
            m_impl->ScheduleDrain();
            return result;
        }

        m_impl->StoreVersion(node);
        m_impl->RemoveWaiterEdges(node);
        node.desiredRevision = desiredRevision;
        node.versionGeneration = versionGeneration->second;
        node.generation = versionGeneration->second;
        node.requestFingerprint = requestFingerprint;
        node.lease = versionLease;
        node.requirements = std::move(requirements);
        node.requestedRequirements = node.requirements;
        node.input = std::move(input);
        node.producedRevision = 0;
        node.payload = {};
        node.resolvedDependencies.clear();
        node.terminalFailure = false;
        node.latestSuccessorNeeded = false;
        node.buildAttempted = false;
        node.error.clear();
        node.gpuSubmissions.reset();
        node.retryAt.reset();
        m_impl->SetState(node, ArtifactReadiness::Missing);
        m_impl->InstallWaiterEdges(node);
        const auto cycle = m_impl->DetectCycle(node);
        if (!cycle.empty()) {
            m_impl->FailCycle(cycle);
        } else {
            std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
            m_impl->SupersedeDependents(key, visited);
            m_impl->QueueNode(node);
        }
        result = { ArtifactRequestStatus::Accepted, node.generation,
            { key, desiredRevision, versionGeneration->second }, versionLease };
        if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
            session->Record("RequestAccepted", key, desiredRevision,
                versionGeneration->second, node.state);
        }
    }
    m_impl->ScheduleDrain();
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
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("Invalidated", key, desiredRevision);
    }
    std::vector<ArtifactRequirement> requirements;
    ArtifactPayload input;
    bool foundNode = false;
    std::uint64_t requestFingerprint = 0;
    {
        std::lock_guard lock(m_impl->mutex);
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
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("Cancelled", key);
    }
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(key);
        if (found == m_impl->nodes.end()) return;
        ++found->second.generation;
        found->second.desired = false;
        found->second.successors.clear();
        found->second.retryAt.reset();
        if ((found->second.state == ArtifactReadiness::CpuReady ||
             found->second.state == ArtifactReadiness::UploadSubmitted) &&
            found->second.waitingGpuSubmissions &&
            m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
        if (found->second.waitingGpuSubmissions) (void)found->second.waitingGpuSubmissions->Cancel();
        found->second.waitingGpuSubmissions.reset();
        found->second.gpuSubmissions.reset();
        found->second.lease.reset();
        m_impl->SetState(found->second, ArtifactReadiness::Cancelled);
        ++m_impl->stats.cancelled;
    }
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::Release(ArtifactKey key) {
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("Released", key);
    }
    {
        std::lock_guard lock(m_impl->mutex);
        const auto found = m_impl->nodes.find(key);
        if (found == m_impl->nodes.end()) return;
        auto& node = found->second;
        m_impl->RemoveWaiterEdges(node);
        ++node.generation;
        node.desired = false;
        node.successors.clear();
        node.retryAt.reset();
        if ((node.state == ArtifactReadiness::CpuReady ||
             node.state == ArtifactReadiness::UploadSubmitted) && node.waitingGpuSubmissions &&
            m_impl->stats.gpuWaiting) --m_impl->stats.gpuWaiting;
        if (node.waitingGpuSubmissions) (void)node.waitingGpuSubmissions->Cancel();
        node.waitingGpuSubmissions.reset();
        node.gpuSubmissions.reset();
        node.lease.reset();
        m_impl->SetState(node, ArtifactReadiness::Cancelled);
        ++m_impl->stats.cancelled;

        const auto dependents = m_impl->waiters.find(key);
        if (!node.buildInFlight && (dependents == m_impl->waiters.end() || dependents->second.empty())) {
            m_impl->nodes.erase(found);
        }
    }
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::MarkPublished(ArtifactKey key, std::uint64_t revision) {
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("Published", key, revision, 0, ArtifactReadiness::Published);
    }
    std::lock_guard lock(m_impl->mutex);
    for (auto& [stored, archived] : m_impl->versions) {
        if (stored.address == key && stored.revision == revision &&
            (archived.readiness == ArtifactReadiness::GpuReady ||
             archived.readiness == ArtifactReadiness::Published)) {
            archived.readiness = ArtifactReadiness::Published;
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
}

void AsyncStateGraph::MarkPublished(ArtifactVersionID version) {
    const auto snapshot = Snapshot(version);
    if (snapshot.readiness == ArtifactReadiness::Missing ||
        snapshot.generation != version.generation) return;
    MarkPublished(version.address, version.revision);
}

void AsyncStateGraph::MarkPublished(std::span<const ArtifactVersionID> versions) {
    if (versions.empty()) return;
    std::lock_guard lock(m_impl->mutex);
    for (const auto& version : versions) {
        if (!version) continue;
        if (auto archived = m_impl->versions.find({ version.address, version.revision,
                version.generation });
            archived != m_impl->versions.end() &&
            archived->second.generation == version.generation &&
            (archived->second.readiness == ArtifactReadiness::UploadSubmitted ||
             archived->second.readiness == ArtifactReadiness::GpuReady ||
             archived->second.readiness == ArtifactReadiness::Published)) {
            archived->second.readiness = ArtifactReadiness::Published;
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
        std::lock_guard lock(m_impl->mutex);
        pending.reserve(m_impl->nodes.size());
        for (const auto& [key, node] : m_impl->nodes) {
            if ((node.state == ArtifactReadiness::CpuReady ||
                 node.state == ArtifactReadiness::UploadSubmitted) &&
                node.waitingGpuSubmissions) {
                pending.push_back(key);
            }
        }
    }
    for (const auto key : pending) m_impl->gpuSignals.push(key);
    m_impl->ScheduleDrain();
}

void AsyncStateGraph::SetReadyCallback(std::function<void(const ArtifactSnapshot&)> callback) {
    std::lock_guard lock(m_impl->mutex);
    if (callback) m_impl->readyCallbacks.insert_or_assign(0, std::move(callback));
    else m_impl->readyCallbacks.erase(0);
}

std::uint64_t AsyncStateGraph::AddReadyCallback(
    std::function<void(const ArtifactSnapshot&)> callback) {
    if (!callback) return 0;
    std::lock_guard lock(m_impl->mutex);
    const auto subscription = ++m_impl->nextReadyCallback;
    m_impl->readyCallbacks.emplace(subscription, std::move(callback));
    return subscription;
}

void AsyncStateGraph::RemoveReadyCallback(std::uint64_t subscription) {
    if (subscription == 0) return;
    std::lock_guard lock(m_impl->mutex);
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
    if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
        session->Record("ObservationRegistered", address, snapshot.revision,
            snapshot.generation, snapshot.readiness, 0,
            std::format("subscription={}", subscription));
    }
    auto weak = std::weak_ptr<Impl>(m_impl);
    return { subscription, sequence->load(std::memory_order_acquire), std::move(snapshot),
        [weak, subscription] {
            if (subscription == 0) return;
            if (const auto graph = weak.lock()) {
                std::lock_guard lock(graph->mutex);
                graph->readyCallbacks.erase(subscription);
                if (auto session = graph->trace.load(std::memory_order_acquire)) {
                    session->Record("ObservationCancelled", {}, 0, 0,
                        ArtifactReadiness::Missing, 0,
                        std::format("subscription={}", subscription));
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
	if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
		session->Record("KindObservationRegistered", { kind, 0, 0 }, 0, 0,
			ArtifactReadiness::Missing, 0, std::format("subscription={}", subscription));
	}
	return { subscription, sequence->load(std::memory_order_acquire), {},
		[weak, subscription] {
			if (subscription == 0) return;
			if (const auto graph = weak.lock()) {
				std::lock_guard lock(graph->mutex);
				graph->readyCallbacks.erase(subscription);
				if (auto session = graph->trace.load(std::memory_order_acquire)) {
					session->Record("ObservationCancelled", {}, 0, 0,
						ArtifactReadiness::Missing, 0,
						std::format("subscription={}", subscription));
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
		std::lock_guard lock(m_impl->mutex);
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
		}
	}
	if (dispatchNow) {
		if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
			session->Record("ExactWaitSatisfied", handle.version.address,
				handle.version.revision, handle.version.generation, snapshot.readiness,
				0, "ready-at-registration");
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
	if (auto session = m_impl->trace.load(std::memory_order_acquire)) {
		session->Record("ExactWaitRegistered", handle.version.address,
			handle.version.revision, handle.version.generation, snapshot.readiness,
			0, std::format("subscription={} milestone={}", subscription,
				static_cast<unsigned>(milestone)));
	}
	auto weak = std::weak_ptr<Impl>(m_impl);
	return { subscription, std::move(snapshot), [weak, subscription, version = handle.version] {
		if (subscription == 0) return;
		if (const auto graph = weak.lock()) {
			std::lock_guard lock(graph->mutex);
			const Impl::StoredVersionKey key{
				version.address, version.revision, version.generation };
			const auto found = graph->exactWaiters.find(key);
			if (found == graph->exactWaiters.end()) return;
			std::erase_if(found->second,
				[subscription](const Impl::ExactWaiter& waiter) {
					return waiter.subscription == subscription;
				});
			if (found->second.empty()) graph->exactWaiters.erase(found);
			if (auto session = graph->trace.load(std::memory_order_acquire)) {
				session->Record("ExactWaitCancelled", version.address,
					version.revision, version.generation, ArtifactReadiness::Missing,
					0, std::format("subscription={}", subscription));
			}
			graph->ScheduleDrain();
		}
	} };
}

ArtifactSnapshot AsyncStateGraph::Snapshot(ArtifactKey key) const {
    std::lock_guard lock(m_impl->mutex);
    const auto found = m_impl->nodes.find(key);
    return found == m_impl->nodes.end() ? ArtifactSnapshot{ key } : m_impl->MakeSnapshot(found->second);
}

ArtifactSnapshot AsyncStateGraph::Snapshot(ArtifactVersionID version) const {
    std::lock_guard lock(m_impl->mutex);
	return m_impl->SnapshotExactLocked(version);
}

ArtifactDiagnostic AsyncStateGraph::Diagnose(ArtifactKey key) const {
    std::lock_guard lock(m_impl->mutex);
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
    std::unordered_set<ArtifactKey, ArtifactKey::Hasher> visited;
    m_impl->AppendBlockerChain(key, visited, result.blockerChain);
    return result;
}

AsyncStateGraphStats AsyncStateGraph::Stats() const {
    std::lock_guard lock(m_impl->mutex);
    auto result = m_impl->stats;
    result.archivedVersions = m_impl->versions.size();
    result.reclaimedVersions = m_impl->reclaimedVersions;
    result.stateCounts.fill(0);
    for (const auto& [_, node] : m_impl->nodes) {
        const auto index = static_cast<std::size_t>(node.state);
        if (index < result.stateCounts.size()) ++result.stateCounts[index];
    }
    return result;
}

std::uint64_t AsyncStateGraph::Outstanding(ArtifactKind kind) const {
    std::lock_guard lock(m_impl->mutex);
    return static_cast<std::uint64_t>(std::ranges::count_if(
        m_impl->nodes, [&](const auto& entry) {
            const auto& node = entry.second;
            if (node.key.kind != kind || !node.desired) return false;
            return node.state != ArtifactReadiness::GpuReady &&
                node.state != ArtifactReadiness::Published &&
                node.state != ArtifactReadiness::Superseded &&
                node.state != ArtifactReadiness::Cancelled &&
                node.state != ArtifactReadiness::Failed;
        }));
}

void AsyncStateGraph::StartTrace(AsyncStateGraphTraceConfig config) {
    if (!m_impl || m_impl->shuttingDown.load(std::memory_order_acquire)) return;
    if (config.maximumEvents == 0) config.maximumEvents = 1;
    auto session = std::make_shared<GraphTraceSession>(config);
    session->Record("TraceStarted", {});
    m_impl->trace.store(std::move(session), std::memory_order_release);
}

bool AsyncStateGraph::TraceActive() const {
    return m_impl && static_cast<bool>(m_impl->trace.load(std::memory_order_acquire));
}

AsyncStateGraphTraceReport AsyncStateGraph::StopTraceAndWriteReport(
    const std::filesystem::path& outputDirectory) {
    AsyncStateGraphTraceReport report;
    if (!m_impl) return report;
    auto session = m_impl->trace.exchange({}, std::memory_order_acq_rel);
    if (!session) return report;
    session->Record("TraceStopped", {});
    return session->Write(outputDirectory);
}

void AsyncStateGraph::WaitIdle() const {
    if (!m_impl) return;
    // Tests, shutdown, and explicit drains define idle as no CPU graph work;
    // submitted GPU versions remain valid schedulable artifacts. Temporarily
    // stop the recovery timer so an intentionally incomplete timeline does not
    // make WaitIdle wait for the GPU.
    m_impl->pauseGpuRecovery.store(true, std::memory_order_release);
    m_impl->scope.Wait();
    m_impl->pauseGpuRecovery.store(false, std::memory_order_release);
    bool resumeRecovery = false;
    {
        std::lock_guard lock(m_impl->mutex);
        resumeRecovery = !m_impl->gpuRecovery.empty();
    }
    if (resumeRecovery) m_impl->ScheduleDrain();
}

void AsyncStateGraph::Shutdown() {
    if (!m_impl || m_impl->shuttingDown.exchange(true)) return;
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
    std::lock_guard lock(m_impl->mutex);
    for (auto& [_, node] : m_impl->nodes) {
        if (node.waitingGpuSubmissions) (void)node.waitingGpuSubmissions->Cancel();
    }
    for (auto& completion : m_impl->completions) {
        if (completion.result.gpuSubmissions) (void)completion.result.gpuSubmissions->Cancel();
    }
    m_impl->pending.clear();
    m_impl->completions.clear();
    ArtifactKey discardedSignal;
    while (m_impl->gpuSignals.try_pop(discardedSignal)) {}
    m_impl->readyCallbacks.clear();
	m_impl->exactWaiters.clear();
}

} // namespace br::render
