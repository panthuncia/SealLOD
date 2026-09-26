#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include <BasicRenderer/Streaming/RendererStateArtifacts.h>
#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace br::render {

struct PublishedGpuBufferVersion;

struct ObjectBufferStateBuildInput {
    std::vector<ObjectBufferDependencyDTO> buffers;
    std::uint64_t coveredMutationGeneration = 0;
    std::uint32_t residentTransformCount = 0;
    std::shared_ptr<const std::vector<SkinnedAssemblyPlacementGPU>> placementRecords;
    std::shared_ptr<const std::vector<PublishedActiveSkinnedPlacement>> activePlacementEntries;
};

void RegisterObjectBufferStateProducer(AsyncStateGraph& graph);

// Coverage of the Geometry root the renderer is currently drawing with.
// Static draw records name mesh-template and CLod rows that shaders resolve only
// through that root, so a draw-records root must not be built until the resident
// root covers them. The owner observes each committed manifest; gates waiting on
// a coverage are woken through the graph's suspension notifier once it is met.
class ResidentGeometryCoverage {
public:
    explicit ResidentGeometryCoverage(std::function<void(std::uint64_t)> notifier)
        : m_notify(std::move(notifier)) {}
    [[nodiscard]] std::uint64_t Resident() const noexcept {
        return m_resident.load(std::memory_order_acquire);
    }
    // Zero when coverage is already resident; otherwise a suspension identity
    // that is notified once it becomes resident.
    [[nodiscard]] std::uint64_t AwaitIdentity(std::uint64_t coverage);
    void Observe(std::uint64_t residentCoverage);

private:
    std::function<void(std::uint64_t)> m_notify;
    std::atomic<std::uint64_t> m_resident{ 0 };
    std::mutex m_mutex;
    std::multimap<std::uint64_t, std::uint64_t> m_waiting;
};

// GeometryCoverageGate{coverage}: ready once the resident Geometry root covers
// `coverage`. A draw-records root holds an exact requirement on its gate.
struct GeometryCoverageGateInput {
    std::uint64_t coverage = 0;
    std::shared_ptr<ResidentGeometryCoverage> resident;
};

struct PublishedGeometryCoverageGate {
    std::uint64_t coverage = 0;
};

void RegisterGeometryCoverageGateProducer(AsyncStateGraph& graph);

} // namespace br::render
