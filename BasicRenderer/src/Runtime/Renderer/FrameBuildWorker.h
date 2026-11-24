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
    // The host's count of the frame epoch's rebuild requests when the build started (GraphHost::RebuildRequests). A larger
    // count at the attach is a request the build does not answer.
    uint64_t hostRequestsAtStart = 0;
    std::chrono::steady_clock::time_point started;
    double prepareMs = 0.0;  // render thread: the inputs (MakeFrameBuild) and the host's extensions
    // The publication the build's resolvers are captured against: the render thread's latest manifest when it started, as
    // that frame's preparation would have used (GraphHost::BuildEpochGraph's capture context).
    std::shared_ptr<const org::ResolverCaptureContext> captureContext;

    // Written by the thread that builds.
    std::unique_ptr<org::GraphHost::EpochGraph> graph;
    // Built on the render thread by the host's own build point (GraphHost::BuildIfRequested), so already in place: no attach.
    bool installed = false;
    std::vector<std::string> pipelineFailures;
    size_t pipelinesDeclared = 0;
    std::vector<std::string> settingsWritten;
    std::string error;  // a failure outside the host's build (it reports its own through the graph)
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
