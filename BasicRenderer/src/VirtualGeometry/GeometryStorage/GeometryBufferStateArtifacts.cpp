#include "VirtualGeometry/GeometryStorage/GeometryBufferStateArtifacts.h"

#include <ranges>
#include <unordered_set>

#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Resources/GloballyIndexedResource.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildGeometryBufferState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<GeometryBufferStateBuildInput>();
    // The CLod residency capacity gate is the only dependency besides the buffers.
    const auto residencyGate = std::ranges::find_if(context.dependencies, [](const ArtifactSnapshot& value) {
        return value.key.kind == ArtifactKind::CLodResidencyCapacityGate;
    });
    const std::size_t gateDependencies = residencyGate != context.dependencies.end() ? 1u : 0u;
    if (!input || input->buffers.empty() ||
        context.dependencies.size() != input->buffers.size() + gateDependencies) {
        return ArtifactBuildResult::Failure("geometry buffer dependency closure incomplete");
    }
    auto state = std::make_shared<PublishedGeometryBufferState>();
    state->buffers = input->buffers;
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Geometry;
    // This is a dependency/catalog cut, not scene membership. Publishing it as
    // a root replaces the StaticScene in the Geometry manifest slot while
    // leaving independently published draw/indirect state active. A subsequent
    // buffer revision can then either erase static membership or make existing
    // non-terrain draws interpret a mismatched (often terrain-dominated) table.
    // StaticScene is the sole Geometry root and retains this exact version in
    // its immutable dependency closure.
    root->publishRoot = false;
    root->fragment.revision = context.revision;
    root->fragment.coverage = input->coveredMutationSequence;
    std::unordered_set<std::uint64_t> variants;
    for (const auto& expected : input->buffers) {
        if (!variants.insert(expected.catalogVariant).second) {
            return ArtifactBuildResult::Failure("geometry buffer catalog variant duplicated");
        }
        const auto dependency = std::ranges::find_if(context.dependencies,
            [&](const ArtifactSnapshot& value) {
                return value.key == expected.key && value.revision == expected.revision;
            });
        const auto dependencyRoot = dependency != context.dependencies.end()
            ? dependency->payload.Get<RendererStateFragmentArtifact>() : nullptr;
        const auto version = dependencyRoot
            ? dependencyRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
        if (!version || !version->resource || version->elementStride != expected.elementStride) {
            return ArtifactBuildResult::Failure("geometry buffer dependency ABI mismatch");
        }
        state->versions.push_back(version);
        auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
        resources->push_back(version->resource);
        root->catalogEntries.emplace_back(PublishedResourceKey{
            PublishedFragmentKind::Geometry, PublishedResourceUsage::ShaderResource,
            0, 0, expected.catalogVariant }, std::move(resources));
        root->fragment.resourceHolds.push_back(version);
    }
    if (residencyGate != context.dependencies.end()) {
        // The non-resident bitset covering every group in this cut's group table.
        const auto residency = residencyGate->payload.Get<RendererStateFragmentArtifact>();
        if (!residency) return ArtifactBuildResult::Failure("CLod residency gate has no storage fragment");
        root->catalogEntries.insert(root->catalogEntries.end(),
            residency->catalogEntries.begin(), residency->catalogEntries.end());
        root->fragment.resourceHolds.insert(root->fragment.resourceHolds.end(),
            residency->fragment.resourceHolds.begin(), residency->fragment.resourceHolds.end());
    }
    root->fragment.payload = ArtifactPayload::Make<PublishedGeometryBufferState>(std::move(state));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterGeometryBufferStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::GeometryBufferState, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "GeometryBufferStateArtifact::Build", BuildGeometryBufferState });
}

} // namespace br::render
