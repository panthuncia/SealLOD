#include "Render/ObjectBufferStateArtifacts.h"

#include <algorithm>
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
    // The geometry-coverage gate authorizes the build; it carries no resource.
    const auto bufferDependencies = std::ranges::count_if(context.dependencies,
        [](const ArtifactSnapshot& dependency) {
            return dependency.key.kind != ArtifactKind::GeometryCoverageGate;
        });
    if (static_cast<std::size_t>(bufferDependencies) != input->buffers.size()) {
        return ArtifactBuildResult::Failure("object buffer dependency closure incomplete");
    }

    std::unordered_set<std::uint64_t> variants;
    auto state = std::make_shared<PublishedObjectBufferState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::DrawRecords;
    root->fragment.revision = context.revision;
    state->buffers = input->buffers;
    state->coveredMutationGeneration = input->coveredMutationGeneration;
    state->residentTransformCount = input->residentTransformCount;
    state->placementRecords = input->placementRecords;
    state->activePlacementEntries = input->activePlacementEntries;

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

ArtifactBuildResult BuildGeometryCoverageGate(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<GeometryCoverageGateInput>();
    if (!input || !input->resident || input->coverage != context.key.primaryID) {
        return ArtifactBuildResult::Failure("geometry coverage gate input invalid");
    }
    if (const auto identity = input->resident->AwaitIdentity(input->coverage); identity != 0) {
        return ArtifactBuildResult::Suspend(ArtifactSuspension::External(identity,
            "draw records wait for a resident Geometry root covering their templates"));
    }
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<PublishedGeometryCoverageGate>(
        std::make_shared<PublishedGeometryCoverageGate>(PublishedGeometryCoverageGate{ input->coverage })));
}

} // namespace

std::uint64_t ResidentGeometryCoverage::AwaitIdentity(std::uint64_t coverage) {
    std::lock_guard lock(m_mutex);
    if (m_resident.load(std::memory_order_acquire) >= coverage) return 0;
    const auto identity = AsyncStateGraph::AllocateSuspensionIdentity();
    m_waiting.emplace(coverage, identity);
    return identity;
}

void ResidentGeometryCoverage::Observe(std::uint64_t residentCoverage) {
    std::vector<std::uint64_t> satisfied;
    {
        std::lock_guard lock(m_mutex);
        if (residentCoverage <= m_resident.load(std::memory_order_acquire)) return;
        m_resident.store(residentCoverage, std::memory_order_release);
        const auto end = m_waiting.upper_bound(residentCoverage);
        for (auto it = m_waiting.begin(); it != end; ++it) satisfied.push_back(it->second);
        m_waiting.erase(m_waiting.begin(), end);
    }
    if (m_notify) for (const auto identity : satisfied) m_notify(identity);
}

void RegisterGeometryCoverageGateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::GeometryCoverageGate, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "GeometryCoverageGate::Build", BuildGeometryCoverageGate });
}

void RegisterObjectBufferStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::DrawRecordPage, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "ObjectBufferStateArtifact::Build", BuildObjectBufferState });
}

std::shared_ptr<const PublishedGpuBufferVersion> PublishedObjectBufferState::FindVersion(
    std::uint64_t catalogVariant) const {
    const auto expected = std::ranges::find(buffers, catalogVariant,
        &ObjectBufferDependencyDTO::catalogVariant);
    if (expected == buffers.end()) return {};
    const auto index = static_cast<std::size_t>(std::distance(buffers.begin(), expected));
    return index < versions.size() ? versions[index] : nullptr;
}

} // namespace br::render
