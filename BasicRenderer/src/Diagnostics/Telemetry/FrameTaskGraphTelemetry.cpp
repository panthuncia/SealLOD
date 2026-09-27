#include "FrameTaskGraphTelemetry.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

namespace br::telemetry {

namespace {
std::mutex g_frameTaskGraphMutex;
bool g_hasActiveFrameTaskGraphSnapshot = false;
std::chrono::steady_clock::time_point g_frameTaskGraphCaptureStart{};
FrameTaskGraphSnapshot g_activeFrameTaskGraphSnapshot{};
FrameTaskGraphSnapshot g_latestFrameTaskGraphSnapshot{};
uint64_t g_frameTaskGraphSequence = 0;
}

void BeginFrameTaskGraphCapture(uint64_t frameNumber, uint8_t frameIndex) {
    std::lock_guard<std::mutex> lock(g_frameTaskGraphMutex);
    g_activeFrameTaskGraphSnapshot = {};
    g_activeFrameTaskGraphSnapshot.frameNumber = frameNumber;
    g_activeFrameTaskGraphSnapshot.frameIndex = frameIndex;
    g_frameTaskGraphCaptureStart = std::chrono::steady_clock::now();
    g_hasActiveFrameTaskGraphSnapshot = true;
}

int32_t RecordFrameTaskNode(
    const char* nodeName,
    CpuTaskDomain domain,
    int32_t dependencyNodeIndex,
    const std::chrono::steady_clock::time_point& nodeStart,
    const std::chrono::steady_clock::time_point& nodeEnd) {
    std::lock_guard<std::mutex> lock(g_frameTaskGraphMutex);
    if (!g_hasActiveFrameTaskGraphSnapshot) {
        return -1;
    }

    if (g_activeFrameTaskGraphSnapshot.nodeCount >= MaxFrameTaskNodes) {
        ++g_activeFrameTaskGraphSnapshot.droppedNodeCount;
        return -1;
    }

    auto& node = g_activeFrameTaskGraphSnapshot.nodes[g_activeFrameTaskGraphSnapshot.nodeCount];
    std::snprintf(node.name, sizeof(node.name), "%s", nodeName != nullptr ? nodeName : "UnnamedTask");
    node.domain = domain;
    node.dependencyNodeIndex = dependencyNodeIndex;
    node.startTimeUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(nodeStart - g_frameTaskGraphCaptureStart).count());
    node.durationUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(nodeEnd - nodeStart).count());

    const int32_t recordedNodeIndex = static_cast<int32_t>(g_activeFrameTaskGraphSnapshot.nodeCount);
    ++g_activeFrameTaskGraphSnapshot.nodeCount;
    return recordedNodeIndex;
}

void PublishFrameTaskGraphSnapshot() {
    std::lock_guard<std::mutex> lock(g_frameTaskGraphMutex);
    if (!g_hasActiveFrameTaskGraphSnapshot) {
        return;
    }

    g_latestFrameTaskGraphSnapshot = g_activeFrameTaskGraphSnapshot;
    ++g_frameTaskGraphSequence;
    g_hasActiveFrameTaskGraphSnapshot = false;
}

bool TryReadFrameTaskGraphSnapshot(uint64_t& inOutSequence, FrameTaskGraphSnapshot& outSnapshot) {
    std::lock_guard<std::mutex> lock(g_frameTaskGraphMutex);
    if (g_frameTaskGraphSequence == inOutSequence) {
        return false;
    }

    outSnapshot = g_latestFrameTaskGraphSnapshot;
    inOutSequence = g_frameTaskGraphSequence;
    return true;
}

} // namespace br::telemetry
