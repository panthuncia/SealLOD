#include "BasicRenderer/Streaming/LightStateArtifacts.h"
#include "Runtime/GraphIntegration/StateProducerRegistrations.h"
#include "Runtime/StateGraph/AsyncStateGraph.h"

#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include "BasicRenderer/Streaming/ViewStateArtifacts.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>

namespace br::render {
namespace {

ArtifactBuildResult BuildLightTable(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<LightTableBuildInput>();
    if (!input || input->revision != context.revision) {
        return ArtifactBuildResult::Failure("light-table input/revision mismatch");
    }
    auto state = std::make_shared<PublishedLightTableState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    state->revision = input->revision;
    state->lightCount = input->lightCount;
    state->lightPagePoolSize = input->lightPagePoolSize;
    state->directionalShadows = input->directionalShadows;
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

    root->kind = PublishedFragmentKind::Lights;
    root->fragment.revision = context.revision;
    root->fragment.payload = ArtifactPayload::Make<PublishedLightTableState>(state);
    root->fragment.resourceHolds.reserve(state->retainedResources.size());
    for (const auto& resource : state->retainedResources) root->fragment.resourceHolds.push_back(resource);
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterLightStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::LightTable, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "LightTableArtifact::Build", BuildLightTable });
}

} // namespace br::render
