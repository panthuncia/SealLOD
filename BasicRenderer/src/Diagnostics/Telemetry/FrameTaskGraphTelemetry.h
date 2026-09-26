#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace br::telemetry {

enum class CpuTaskDomain : uint8_t {
    MainThread = 0,
    Worker = 1,
    IOService = 2,
    BackgroundService = 3,
};

struct FrameTaskNodeSnapshot {
    char name[64]{};
    CpuTaskDomain domain = CpuTaskDomain::MainThread;
    int32_t dependencyNodeIndex = -1;
    uint64_t startTimeUs = 0;
    uint64_t durationUs = 0;
};

inline constexpr uint32_t MaxFrameTaskNodes = 256;

struct FrameTaskGraphSnapshot {
    uint64_t frameNumber = 0;
    uint8_t frameIndex = 0;
    uint32_t nodeCount = 0;
    uint32_t droppedNodeCount = 0;
    std::array<FrameTaskNodeSnapshot, MaxFrameTaskNodes> nodes{};
};

void BeginFrameTaskGraphCapture(uint64_t frameNumber, uint8_t frameIndex);
int32_t RecordFrameTaskNode(
    const char* nodeName,
    CpuTaskDomain domain,
    int32_t dependencyNodeIndex,
    const std::chrono::steady_clock::time_point& nodeStart,
    const std::chrono::steady_clock::time_point& nodeEnd);
void PublishFrameTaskGraphSnapshot();
bool TryReadFrameTaskGraphSnapshot(uint64_t& inOutSequence, FrameTaskGraphSnapshot& outSnapshot);

} // namespace br::telemetry