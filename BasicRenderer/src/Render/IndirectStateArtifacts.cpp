#include "Render/IndirectStateArtifacts.h"

#include <algorithm>
#include <limits>
#include <ranges>

#include "Render/IndirectCommand.h"
#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildViewLifetime(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<ViewLifetimeArtifact>();
    if (!input || input->viewID != context.key.primaryID ||
        input->lifetimeRevision != context.revision) {
        return ArtifactBuildResult::Failure("view-lifetime identity/revision mismatch");
    }
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<ViewLifetimeArtifact>(input));
}

std::uint32_t RoundUp(std::uint32_t value, std::uint32_t increment) {
    increment = (std::max)(increment, 1u);
    const auto result = ((static_cast<std::uint64_t>(value) + increment - 1u) / increment) * increment;
    return static_cast<std::uint32_t>((std::min)(result,
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())));
}

ArtifactBuildResult BuildIndirectState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<IndirectStateBuildInput>();
    if (!input) {
        return ArtifactBuildResult::Failure("indirect-state immutable input missing");
    }

    Resource::ScopedECSRegistrationSuppression suppressECS;
    auto state = std::make_shared<PublishedIndirectState>();
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::IndirectWorkloads;
    root->catalogOwnerMask = PublishedFragmentMask(PublishedFragmentKind::IndirectWorkloads) |
        PublishedFragmentMask(PublishedFragmentKind::ActiveDrawLists);
    root->fragment.revision = context.revision;
	root->fragment.dependencyClosure = context.dependencies;

    for (const auto& workload : input->workloads) {
        const auto logicalCount = static_cast<std::uint64_t>(workload.activeEntries.size());
        const auto safeCount64 = (std::min)({ static_cast<std::uint64_t>(workload.requestedCount),
            logicalCount, static_cast<std::uint64_t>(workload.residentDrawRecordCount) });
        const auto safeCount = static_cast<std::uint32_t>(safeCount64);
        if (safeCount == 0) continue;

        const auto capacity = (std::max)(workload.minimumCapacity,
            safeCount == 0u ? 0u : RoundUp(safeCount, input->incrementSize));
        std::shared_ptr<org::GloballyIndexedResource> activeBuffer;
        if (input->materializeResources) {
            const auto dependency = std::ranges::find_if(context.dependencies, [&](const auto& candidate) {
                return candidate.key == workload.activeListArtifactKey;
            });
            if (dependency == context.dependencies.end()) {
                return ArtifactBuildResult::Failure("indirect workload active-list dependency missing");
            }
            const auto dependencyRoot = dependency->payload.Get<RendererStateFragmentArtifact>();
            const auto version = dependencyRoot
                ? dependencyRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
            if (!version || !version->resource || version->elementStride != sizeof(ActiveDrawEntryDTO)) {
                return ArtifactBuildResult::Failure("indirect workload active-list dependency type/ABI mismatch");
            }
            activeBuffer = version->resource;
        }
        for (const auto viewID : input->viewIDs) {
            std::shared_ptr<org::GloballyIndexedResource> dynamicArgs;
            if (input->materializeResources) {
                const auto argument = std::ranges::find_if(workload.argumentArtifacts,
                    [viewID](const auto& candidate) { return candidate.viewID == viewID; });
                if (argument == workload.argumentArtifacts.end()) {
                    return ArtifactBuildResult::Failure("indirect argument artifact identity missing");
                }
                const auto dependency = std::ranges::find_if(context.dependencies, [&](const auto& candidate) {
                    return candidate.key == argument->key;
                });
                const auto dependencyRoot = dependency != context.dependencies.end()
                    ? dependency->payload.Get<RendererStateFragmentArtifact>() : nullptr;
                const auto version = dependencyRoot
                    ? dependencyRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
                if (!version || !version->resource ||
                    version->elementStride != sizeof(DispatchMeshIndirectCommand) ||
                    version->capacity < capacity) {
                    return ArtifactBuildResult::Failure("indirect argument dependency type/capacity mismatch");
                }
                dynamicArgs = version->resource;
            }
            state->workloads.push_back(PublishedIndirectWorkload{
                viewID, workload.key, dynamicArgs, activeBuffer, safeCount, capacity,
                workload.activeListRevision });

            if (dynamicArgs) {
                auto argsResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
                argsResources->push_back(dynamicArgs);
                root->catalogEntries.emplace_back(PublishedResourceKey{
                    PublishedFragmentKind::IndirectWorkloads, PublishedResourceUsage::IndirectArguments,
                    workload.key.renderPhase.hash, viewID,
                    static_cast<std::uint64_t>(workload.key.compileFlags) |
                        (static_cast<std::uint64_t>(workload.key.skinnedShadowCaster) << 62u) |
                        (static_cast<std::uint64_t>(workload.key.clodOnly) << 63u) }, argsResources);
            }
        }

        if (activeBuffer) {
            auto activeResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
            activeResources->push_back(activeBuffer);
            root->catalogEntries.emplace_back(PublishedResourceKey{
                PublishedFragmentKind::ActiveDrawLists, PublishedResourceUsage::ActiveDrawList,
                workload.key.renderPhase.hash, 0,
                static_cast<std::uint64_t>(workload.key.compileFlags) |
                    (static_cast<std::uint64_t>(workload.key.skinnedShadowCaster) << 62u) |
                    (static_cast<std::uint64_t>(workload.key.clodOnly) << 63u) }, activeResources);
        }
    }

    root->fragment.payload = ArtifactPayload::Make<PublishedIndirectState>(std::move(state));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

std::vector<const PublishedIndirectWorkload*> PublishedIndirectState::Find(
    std::uint64_t viewID, const RenderPhase& phase, bool clodOnly) const {
    std::vector<const PublishedIndirectWorkload*> result;
    for (const auto& workload : workloads) {
        if (workload.viewID == viewID && workload.key.renderPhase == phase &&
            workload.key.clodOnly == clodOnly && workload.count != 0) result.push_back(&workload);
    }
    return result;
}

void RegisterIndirectStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::ViewLifetime, {
        TaskLane::Streaming, TaskDomain::RendererState,
        "ViewLifetimeArtifact::Build", BuildViewLifetime });
    graph.RegisterProducer(ArtifactKind::IndirectWorkload, {
        TaskLane::Streaming, TaskDomain::RendererState,
        "IndirectStateArtifact::Build", BuildIndirectState });
}

} // namespace br::render
