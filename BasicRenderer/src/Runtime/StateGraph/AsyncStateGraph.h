#pragma once

#include <BasicRenderer/Streaming/ArtifactTypes.h>
#include "Runtime/StateGraph/GraphTrace.h"
#include <ORGModuleServices/Async/StateGraph.h>

namespace br::render {

using RendererGraphBinding = org::async::StateGraphTypes<ArtifactKind, RendererArtifactScheduling,
    kArtifactKindCount, AsyncStateGraphTraceEventID, AsyncStateGraphTraceReport>;

class AsyncStateGraph;
struct AsyncStateGraphHostHooks {
    AsyncStateGraphHostHooks();
    // Fixed for the graph lifetime; the coordinator and posted-request callers
    // may read these without synchronization. Generic defaults retain exact
    // content even for replaceable intents. The renderer installs its legacy
    // policy explicitly in MakeRendererGraphHooks().
    std::array<org::async::ArtifactKindPolicy, kArtifactKindCount> artifactPolicies{};
    org::async::GraphSchedulingLayout scheduling;
    std::vector<org::async::TaskDispatch> producerDispatch;
    // Runtime callbacks must not throw. Trace-session creation runs on the
    // explicit StartTrace control path and may fail like any other allocation.
    // The host interprets its payloads and wakes its resource brokers.
    std::function<void()> versionsRetired;
    std::function<std::shared_ptr<graph_detail::GraphTraceSession>(AsyncStateGraphTraceConfig)> createTraceSession;
    std::function<void(AsyncStateGraph&, ArtifactKey, std::uint64_t, std::uint64_t,
        const ArtifactPayload&)> traceAcceptedInput;
};


class AsyncStateGraph final : public org::async::StateGraph<RendererGraphBinding> {
    using Base = org::async::StateGraph<RendererGraphBinding>;
public:
    explicit AsyncStateGraph(TaskSchedulerManager& scheduler, std::string_view name = "RendererStateGraph");
    explicit AsyncStateGraph(std::shared_ptr<org::async::GraphScheduler> scheduler,
        std::string_view name = "RendererStateGraph", AsyncStateGraphHostHooks hooks = {});
    ~AsyncStateGraph();
    AsyncStateGraph(const AsyncStateGraph&) = delete;
    AsyncStateGraph& operator=(const AsyncStateGraph&) = delete;

private:
    static org::async::StateGraphHooks<RendererGraphBinding> ConvertHooks(AsyncStateGraphHostHooks hooks);
};

} // namespace br::render
