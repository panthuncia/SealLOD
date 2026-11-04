#pragma once

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include "Runtime/StateGraph/GraphDiagnostics.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

// A graph trace capture: the events exactly as the running renderer recorded them. Stopping a trace only dumps
// this. The CSV, summary and Chrome reports are generated from it afterwards (GraphTraceReport).
namespace br::render {

using graph_detail::GraphMutexCounts;
using graph_detail::GraphMutexPhase;

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
static_assert(std::is_trivially_copyable_v<GraphMutexAggregate>);

inline constexpr std::array<char, 8> kGraphTraceMagic{ 'S', 'A', 'R', 'P', 'G', 'T', 'R', 'C' };
// Bump when the event layout or the meaning of any event id or payload changes.
inline constexpr std::uint32_t kGraphTraceFormatVersion = 1;

struct GraphTraceFileHeader {
    std::array<char, 8> magic = kGraphTraceMagic;
    std::uint32_t formatVersion = kGraphTraceFormatVersion;
    std::uint32_t eventBytes = sizeof(GraphTraceEvent);
    std::uint32_t eventIdCount = static_cast<std::uint32_t>(AsyncStateGraphTraceEventID::Count);
    std::uint32_t mutexPhaseCount = static_cast<std::uint32_t>(GraphMutexPhase::Count);
    std::uint32_t threadCount = 0;
    std::uint32_t schedulerEventsCaptured = 0;
    std::int64_t elapsedMicros = 0;
    std::uint64_t droppedEvents = 0;
};
static_assert(std::is_trivially_copyable_v<GraphTraceFileHeader>);

// Followed in the file by its events.
struct GraphTraceFileThread {
    std::uint64_t eventCount = 0;
    std::array<char, 64> name{};
};
static_assert(std::is_trivially_copyable_v<GraphTraceFileThread>);

using GraphMutexAggregates = std::array<GraphMutexAggregate, static_cast<std::size_t>(GraphMutexPhase::Count)>;

inline constexpr const char* kGraphTraceCaptureFileName = "async_state_graph_trace.bin";

struct AsyncStateGraphTraceExpandOptions {
    // Writes async_state_graph_trace.json, which is roughly twice the size of the events CSV.
    bool chromeTrace = false;
};

// Writes the reports next to each other in outputDirectory. Throws std::runtime_error on an unreadable capture, or
// one written by a build with a different event format.
AsyncStateGraphTraceReport ExpandAsyncStateGraphTrace(const std::filesystem::path& capture,
    const std::filesystem::path& outputDirectory, const AsyncStateGraphTraceExpandOptions& options = {});

} // namespace br::render
