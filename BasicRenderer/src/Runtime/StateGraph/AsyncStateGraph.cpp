#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <ORGModuleServices/Async/Detail/StateGraphImplementation.h>

// Instantiate the shared algorithm once for the renderer's strongly typed API.
template class org::async::StateGraph<br::render::RendererGraphBinding>;

namespace br::render {

org::async::StateGraphHooks<RendererGraphBinding> AsyncStateGraph::ConvertHooks(AsyncStateGraphHostHooks hooks) {
    org::async::StateGraphHooks<RendererGraphBinding> shared;
    shared.artifactPolicies = std::move(hooks.artifactPolicies);
    shared.scheduling = hooks.scheduling;
    shared.producerDispatch = std::move(hooks.producerDispatch);
    shared.versionsRetired = std::move(hooks.versionsRetired);
    shared.createTraceSession = std::move(hooks.createTraceSession);
    if (hooks.traceAcceptedInput) {
        shared.traceAcceptedInput = [callback = std::move(hooks.traceAcceptedInput)](
            Base& graph, ArtifactKey key, std::uint64_t revision, std::uint64_t generation,
            const ArtifactPayload& input) {
            callback(static_cast<AsyncStateGraph&>(graph), key, revision, generation, input);
        };
    }
    return shared;
}

AsyncStateGraph::AsyncStateGraph(std::shared_ptr<org::async::GraphScheduler> scheduler,
    std::string_view name, AsyncStateGraphHostHooks hooks)
    : Base(std::move(scheduler), name, ConvertHooks(std::move(hooks))) {}

AsyncStateGraph::~AsyncStateGraph() {
    // Finish callbacks while the derived compatibility wrapper still exists.
    Shutdown();
}

std::uint64_t AllocateArtifactSuspensionIdentity() noexcept {
    return org::async::AllocateArtifactSuspensionIdentity();
}

} // namespace br::render
