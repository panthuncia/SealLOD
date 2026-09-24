#include "Render/GeometryResidencyStateArtifacts.h"

#include <algorithm>

#include "Render/PublishedRendererState.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildGeometryResidencyState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<GeometryResidencyDeltaInput>();
    if (!input || context.revision == 0) {
        return ArtifactBuildResult::Failure("geometry-residency input mismatch");
    }

    auto state = std::make_shared<PublishedGeometryResidencyState>();
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind != ArtifactKind::GeometryResidency) continue;
        const auto predecessorRoot = dependency.payload.Get<RendererStateFragmentArtifact>();
        const auto predecessor = predecessorRoot
            ? predecessorRoot->fragment.payload.Get<PublishedGeometryResidencyState>() : nullptr;
        if (predecessor) {
            *state = *predecessor;
            break;
        }
    }

    // Every published state keeps activeRanges sorted by groupsBase, so a delta
    // edits the predecessor in place instead of scanning and re-sorting it.
    if (input->kind == GeometryResidencyDeltaKind::Reset) {
        state->activeRanges = input->resetRanges;
        std::ranges::sort(state->activeRanges, {}, &GeometryResidencyRange::groupsBase);
    } else {
        auto existing = std::ranges::lower_bound(state->activeRanges,
            input->range.groupsBase, {}, &GeometryResidencyRange::groupsBase);
        const bool found = existing != state->activeRanges.end() &&
            existing->groupsBase == input->range.groupsBase;
        if (input->kind == GeometryResidencyDeltaKind::Remove) {
            if (found) state->activeRanges.erase(existing);
        } else if (input->range.groupCount != 0) {
            if (found) *existing = input->range;
            else state->activeRanges.insert(existing, input->range);
        }
    }

    if (input->pagePool) state->pagePool = input->pagePool;
    if (input->slabResources) state->slabResources = input->slabResources;
    state->storageGeneration = input->storageGeneration;

    state->revision = context.revision;
    state->maxTraversalDepth = 0;
    state->maxGroupIndex = 0;
    for (const auto& range : state->activeRanges) {
        state->maxTraversalDepth = std::max(state->maxTraversalDepth, range.maxTraversalDepth);
        state->maxGroupIndex = std::max(state->maxGroupIndex,
            range.groupsBase + range.groupCount);
    }

    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::GeometryResidency;
    root->fragment.revision = context.revision;
    // The predecessor is build history, not a publication dependency. The
    // successor owns a complete immutable value and can retire independently.
    root->fragment.payload = ArtifactPayload::Make<PublishedGeometryResidencyState>(state);
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterGeometryResidencyStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::GeometryResidency, {
        TaskLane::Streaming, TaskDomain::GraphPublication,
        "GeometryResidencyStateArtifact::Build", BuildGeometryResidencyState });
}

} // namespace br::render
