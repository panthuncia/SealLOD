#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "OpenRenderGraph/GraphHost.h"
#include "BasicRenderer/Runtime/Detail/FrameRevisions.h"

namespace br::render {

struct FrameBuild;

// One frame-graph build, from its start on the render thread to its attach there (docs/frame_graph_composition_plan.md,
// M3 step 9). The render thread fills the first part, the thread that builds the second; the render thread reads that only
// once the build is done.
struct FrameBuildJob {
    std::shared_ptr<FrameBuild> build;
    // What the build answers: given back to the renderer's requests if the build is dropped before its attach.
    FrameRevisions::Pending requests;
    std::string reason;
    uint64_t serial = 0;
    bool onRenderThread = false;
    // It answers a resolution request (Renderer::ApplyResolutionRequest): at most one such build is in flight.
    bool resolution = false;
    // The host's count of each built host epoch's rebuild requests when the build started (GraphHost::RebuildRequests), as
    // FrameBuild::hostEpochs. A larger count at the attach is a request the build does not answer.
    std::vector<uint64_t> hostRequestsAtStart;
    std::chrono::steady_clock::time_point started;
    double prepareMs = 0.0;  // render thread: the inputs (MakeFrameBuild) and the host's extensions
    // The publication the build's resolvers are captured against: the render thread's latest manifest when it started, as
    // that frame's preparation would have used (GraphHost::BuildEpochGraph's capture context).
    std::shared_ptr<const org::ResolverCaptureContext> captureContext;
    // Staged recordings (M6 step 6, GraphHost::RecordStagedCopies): the frame the kept epochs' copies are recorded for on the
    // build's thread, standing in for the first frames after the attach: the build's update values (FrameBuildInputs::lastUpdate)
    // with the last frame's render values. Null when there are none (the first build, a view that changed): nothing is recorded
    // ahead, and the copies' first runs record.
    std::shared_ptr<const org::IHostExecutionData> stagedFrame;
    // The same for the new graph's runs after its first (M7 step 2, RecordStagedCopies by run): the views' depth histories valid
    // (a resize's new depth maps have none at the first run only) and UpdateContext::stagedRun 1. Null with stagedFrame.
    std::shared_ptr<const org::IHostExecutionData> stagedLaterFrame;
    float stagedDeltaTime = 0.0f;
    // Its attach resizes the swapchain, which waits for every frame in flight: the new graphs' runs then take their copies in run
    // order (GraphHost::RecordStagedCopies by run), so only the copies of the first runs are worth recording ahead.
    bool drainsAtAttach = false;
    // Its attach gives the primary view the build's depth maps (a new render size): the view manager's depth map group changes
    // then, and Post's depth history pass declares again at its first run (a new executable), so Post's copies would be stale.
    bool swapsPrimaryDepth = false;

    // Written by the thread that builds: one graph per host epoch of the build (FrameBuild::hostEpochs), in that order.
    struct EpochGraphBuild {
        uint32_t hostEpoch = 0;
        std::unique_ptr<org::GraphHost::EpochGraph> graph;
    };
    std::vector<EpochGraphBuild> graphs;
    // Graphs whose transients the build placed (GraphHost::PlaceEpochGraphs), and how long that took on the build's thread.
    size_t graphsPlacedAtBuild = 0;
    double placementMs = 0.0;
    // Each kept epoch's staged recordings (GraphHost::RecordStagedCopies), after the build's pipelines are in.
    struct StagedRecordings {
        uint32_t hostEpoch = 0;
        org::GraphHost::StagedRecordings result;
    };
    std::vector<StagedRecordings> staged;
    // Built on the render thread by the host's own build point (GraphHost::BuildIfRequested), so already in place: no attach.
    bool installed = false;
    std::vector<std::string> pipelineFailures;
    size_t pipelinesDeclared = 0;
    std::vector<std::string> settingsWritten;
    std::string error;  // a failure outside the host's build (it reports its own through the graph)
    // Its epoch check waits for its first frame: built in place, it had no schedule yet (Renderer::CheckFrameEpochInterface).
    bool epochCheckDeferred = false;
    double buildMs = 0.0;          // the whole build, pipeline waits included
    double pipelineWaitMs = 0.0;   // waiting for the build's own pipelines
};

// The renderer's frame-graph build thread. It builds one job at a time and holds the finished one until the render thread
// takes it. It is a thread of its own, never a task service worker: a build blocks while its pipelines compile on the task
// service (GraphHost::BuildEpochGraph).
class FrameBuildWorker {
public:
    using Run = std::function<void(FrameBuildJob&)>;
    explicit FrameBuildWorker(Run run);
    // Waits for the job in hand.
    ~FrameBuildWorker();
    FrameBuildWorker(const FrameBuildWorker&) = delete;
    FrameBuildWorker& operator=(const FrameBuildWorker&) = delete;

    // Render thread: hands a job over. Only while Idle.
    void Start(std::shared_ptr<FrameBuildJob> job);
    // Whether no job is in hand (none running, none finished and not taken).
    bool Idle() const;
    // The finished job, taken; null while it runs or with none.
    std::shared_ptr<FrameBuildJob> TakeFinished();
    // Blocks until the job in hand finished and takes it; null with none.
    std::shared_ptr<FrameBuildJob> Wait();

private:
    void Loop();

    Run m_run;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::shared_ptr<FrameBuildJob> m_job;
    bool m_finished = false;
    bool m_stop = false;
    std::thread m_thread;
};

} // namespace br::render
