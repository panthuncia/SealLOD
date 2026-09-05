#include "Render/ObjectBufferStateArtifacts.h"

#include <ranges>
#include <unordered_set>

#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildObjectBufferState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<ObjectBufferStateBuildInput>();
    if (!input || input->buffers.empty()) {
        return ArtifactBuildResult::Failure("object buffer state input missing");
    }
    if (context.dependencies.size() != input->buffers.size()) {
        return ArtifactBuildResult::Failure("object buffer dependency closure incomplete");
    }

    std::unordered_set<std::uint64_t> variants;
    auto state = std::make_shared<PublishedObjectBufferState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::DrawRecords;
    root->fragment.revision = context.revision;
    state->buffers = input->buffers;
    state->coveredMutationGeneration = input->coveredMutationGeneration;

    for (const auto& expected : input->buffers) {
        if (!variants.insert(expected.catalogVariant).second) {
            return ArtifactBuildResult::Failure("object buffer catalog variant duplicated");
        }
        const auto dependency = std::ranges::find_if(context.dependencies,
            [&](const ArtifactSnapshot& value) {
                return value.key == expected.key && value.revision == expected.revision;
            });
        const auto dependencyRoot = dependency != context.dependencies.end()
            ? dependency->payload.Get<RendererStateFragmentArtifact>() : nullptr;
        const auto version = dependencyRoot
            ? dependencyRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
        if (!version || !version->resource ||
            version->elementStride != expected.elementStride ||
            version->revision != expected.revision) {
            return ArtifactBuildResult::Failure("object buffer dependency ABI/revision mismatch");
        }
        state->versions.push_back(version);
        auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
        resources->push_back(version->resource);
        root->catalogEntries.emplace_back(PublishedResourceKey{
            PublishedFragmentKind::DrawRecords,
            PublishedResourceUsage::ShaderResource,
            0, 0, expected.catalogVariant }, std::move(resources));
        root->fragment.resourceHolds.push_back(version);
    }
    root->fragment.payload = ArtifactPayload::Make<PublishedObjectBufferState>(std::move(state));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterObjectBufferStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::DrawRecordPage, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "ObjectBufferStateArtifact::Build", BuildObjectBufferState });
}

} // namespace br::render
