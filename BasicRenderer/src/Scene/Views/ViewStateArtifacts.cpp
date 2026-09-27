#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include "Runtime/StateGraph/AsyncStateGraph.h"

#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

namespace br::render {
namespace {

ArtifactBuildResult BuildViewFamily(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<ViewFamilyBuildInput>();
    if (!input || input->revision != context.revision) {
        return ArtifactBuildResult::Failure("view-family input/revision mismatch");
    }
    auto state = std::make_shared<PublishedViewFamilyState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    state->revision = input->revision;
    state->cameraBufferSize = input->cameraBufferSize;
    state->resourceLayoutRevision = input->resourceLayoutRevision;
    state->views = input->views;
    state->retainedResources = input->retainedResources;

    root->kind = PublishedFragmentKind::Views;
    root->fragment.revision = context.revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedViewFamilyState>(state);
    root->fragment.resourceHolds.reserve(state->retainedResources.size());
    for (const auto& resource : state->retainedResources) root->fragment.resourceHolds.push_back(resource);
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterViewStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::ViewFamily, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "ViewFamilyArtifact::Build", BuildViewFamily });
}

} // namespace br::render
