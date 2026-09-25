#include "Render/PoseStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildPoseState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<PoseStateBuildInput>();
    if (!input || input->activeInstanceRevision != context.revision) {
        return ArtifactBuildResult::Failure("pose-state input/revision mismatch");
    }
    auto state = std::make_shared<PublishedPoseState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    state->activeInstanceRevision = input->activeInstanceRevision;
    state->activeInstances = input->activeInstances;
    state->retainedResources = input->retainedResources;
    state->tableImages = input->tableImages;
    for (const auto& dependency : context.dependencies) {
        const auto dependencyRoot = dependency.payload.Get<RendererStateFragmentArtifact>();
        const auto version = dependencyRoot
            ? dependencyRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
        if (!version || !version->resource) continue;
        state->tableVersions.push_back(version);
        root->fragment.resourceHolds.push_back(version);
        root->catalogEntries.insert(root->catalogEntries.end(),
            dependencyRoot->catalogEntries.begin(), dependencyRoot->catalogEntries.end());
    }

    root->kind = PublishedFragmentKind::Poses;
    root->fragment.revision = context.revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedPoseState>(state);
    root->fragment.resourceHolds.reserve(state->retainedResources.size());
    for (const auto& resource : state->retainedResources) root->fragment.resourceHolds.push_back(resource);
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterPoseStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::PoseState, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "PoseStateArtifact::Build", BuildPoseState });
}

} // namespace br::render
