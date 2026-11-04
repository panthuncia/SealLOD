#include "Runtime/StateGraph/RendererGraphAdapters.h"
#include "Runtime/StateGraph/GraphTrace.h"
#include "Runtime/StateGraph/RendererGraphTraceFormat.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <thread>
#include <string_view>
#include <tbb/concurrent_queue.h>
#include <tracy/Tracy.hpp>

namespace br::render {
namespace {
using namespace graph_detail;
using TaskTraceEvent = org::async::TaskTraceEvent;
using TaskTraceEventID = org::async::TaskTraceEventID;

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

// The renderer names its threads through Tracy.
std::array<char, 64> CurrentThreadTraceName() {
    std::array<char, 64> result{};
    const std::string_view name = tracy::GetThreadName(tracy::GetThreadHandle());
    std::copy_n(name.begin(), (std::min)(name.size(), result.size() - 1), result.begin());
    return result;
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
        std::array<char, 64> name{};
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

    // Runs on the thread that stops the trace, so it only dumps what was recorded. ExpandAsyncStateGraphTrace does
    // everything else, later.
    AsyncStateGraphTraceReport Write(const std::filesystem::path& directory) {
        std::vector<std::shared_ptr<TraceShard>> shards;
        std::shared_ptr<TraceShard> shard;
        while (m_shards.try_pop(shard)) shards.push_back(std::move(shard));

        GraphTraceFileHeader header;
        header.threadCount = static_cast<std::uint32_t>(shards.size());
        header.schedulerEventsCaptured = m_config.includeSchedulerEvents ? 1 : 0;
        GraphMutexAggregates mutexAggregates{};
        AsyncStateGraphTraceReport report;
        for (const auto& traceShard : shards) {
            header.droppedEvents += traceShard->dropped;
            for (const auto& chunk : traceShard->chunks) report.capturedEvents += chunk->size;
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
        report.droppedEvents = header.droppedEvents;
        report.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - m_started);
        header.elapsedMicros = report.elapsed.count();

        std::filesystem::create_directories(directory);
        report.rawCapture = directory / kGraphTraceCaptureFileName;
        std::ofstream output(report.rawCapture, std::ios::binary | std::ios::trunc);
        const auto writeValue = [&output](const auto& value) {
            output.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        writeValue(header);
        writeValue(mutexAggregates);
        for (const auto& traceShard : shards) {
            GraphTraceFileThread threadHeader;
            threadHeader.name = traceShard->name;
            for (const auto& chunk : traceShard->chunks) threadHeader.eventCount += chunk->size;
            writeValue(threadHeader);
            for (const auto& chunk : traceShard->chunks) {
                output.write(reinterpret_cast<const char*>(chunk->events.data()),
                    static_cast<std::streamsize>(chunk->size * sizeof(GraphTraceEvent)));
            }
        }
        output.close();
        if (!output) throw std::runtime_error("Failed to write graph trace capture: " + report.rawCapture.string());
        return report;
    }

    const AsyncStateGraphTraceConfig& Config() const { return m_config; }

private:
    TraceShard* ThreadShard() {
        struct CacheEntry { const RendererGraphTraceSession* session = nullptr; std::uint64_t id = 0; TraceShard* shard = nullptr; };
        thread_local CacheEntry cache;
        if (cache.session == this && cache.id == m_sessionID) return cache.shard;
        auto shard = std::make_shared<TraceShard>();
        shard->name = CurrentThreadTraceName();
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
