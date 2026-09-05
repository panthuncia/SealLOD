#include "Render/IndirectStateArtifacts.h"

#include <algorithm>
#include <limits>
#include <ranges>

#include "Render/IndirectCommand.h"
#include "Render/ObjectBufferStateArtifacts.h"
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
	// ReadyGate dependencies authorize this build but do not form an exact
	// publication constraint. Keeping the material root in the immutable closure
	// pinned the active manifest to the material revision that happened to be
	// ready when indirect state was built, preventing every later material table
	// from publishing. View lifetimes have the same gate-only semantics. Buffer
	// artifacts remain in the closure because they are exact resource inputs.
	// DrawRecordPage is different from the other ready gates: active-list entries
	// carry generations that must be interpreted with the exact draw-record and
	// visibility-generation snapshot selected for this indirect root. Retain that
	// resolved root in the publication closure so the manifest advances both
	// fragments atomically. BuildManifest keeps historical ready roots available,
	// so a newer desired DrawRecordPage does not invalidate this coherent pair.
	for (const auto& dependency : context.dependencies) {
		if (dependency.key.kind == ArtifactKind::MaterialTable ||
			dependency.key.kind == ArtifactKind::ViewLifetime) continue;
		root->fragment.dependencyClosure.push_back(dependency);
	}

    const auto drawRecords = std::ranges::find_if(context.dependencies,
        [](const ArtifactSnapshot& dependency) {
            return dependency.key.kind == ArtifactKind::DrawRecordPage;
        });
    const auto drawRoot = drawRecords != context.dependencies.end()
        ? drawRecords->payload.Get<RendererStateFragmentArtifact>() : nullptr;
    if (!drawRoot) {
        return ArtifactBuildResult::Failure(
            "indirect workload exact draw-record dependency missing");
    }
    state->drawRecordsRoot = drawRecords->Version();
    const auto visibility = std::ranges::find_if(drawRoot->catalogEntries,
        [](const auto& entry) {
            return entry.first.owner == PublishedFragmentKind::DrawRecords &&
                entry.first.usage == PublishedResourceUsage::ShaderResource &&
                entry.first.variant == kObjectVisibilityGenerationVariant;
        });
    if (visibility == drawRoot->catalogEntries.end() || !visibility->second ||
        visibility->second->empty()) {
        return ArtifactBuildResult::Failure(
            "indirect workload visibility-generation dependency missing");
    }
    state->visibilityGenerations = std::dynamic_pointer_cast<org::GloballyIndexedResource>(
        visibility->second->front());
    if (!state->visibilityGenerations) {
        return ArtifactBuildResult::Failure(
            "indirect workload visibility-generation dependency type mismatch");
    }

    for (const auto& workload : input->workloads) {
        const auto logicalCount = workload.logicalEntryCount;
        const auto safeCount64 = (std::min)({ static_cast<std::uint64_t>(workload.requestedCount),
            logicalCount, static_cast<std::uint64_t>(workload.residentDrawRecordCount) });
        const auto safeCount = static_cast<std::uint32_t>(safeCount64);
        if (safeCount == 0) continue;

        const auto capacity = (std::max)(workload.minimumCapacity,
            safeCount == 0u ? 0u : RoundUp(safeCount, input->incrementSize));
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
        const auto activeBuffer = version->resource;
        state->activeListVersions.push_back({ workload.activeListArtifactKey.primaryID, version });
        for (const auto viewID : input->viewIDs) {
            std::shared_ptr<org::GloballyIndexedResource> dynamicArgs;
            const auto argument = std::ranges::find_if(workload.argumentArtifacts,
                [viewID](const auto& candidate) { return candidate.viewID == viewID; });
            // Shadow/reflection/probe views participate in culling, but do
            // not execute ForwardRenderPass' indirect command buffers.
            // Their workload rows intentionally carry no argument artifact.
            if (argument != workload.argumentArtifacts.end()) {
                const auto argumentDependency = std::ranges::find_if(context.dependencies, [&](const auto& candidate) {
                    return candidate.key == argument->key;
                });
                const auto argumentRoot = argumentDependency != context.dependencies.end()
                    ? argumentDependency->payload.Get<RendererStateFragmentArtifact>() : nullptr;
                const auto argumentVersion = argumentRoot
                    ? argumentRoot->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
                if (!argumentVersion || !argumentVersion->resource ||
                    argumentVersion->elementStride != sizeof(DispatchMeshIndirectCommand) ||
                    argumentVersion->capacity < capacity) {
                    return ArtifactBuildResult::Failure("indirect argument dependency type/capacity mismatch");
                }
                dynamicArgs = argumentVersion->resource;
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
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "ViewLifetimeArtifact::Build", BuildViewLifetime });
    graph.RegisterProducer(ArtifactKind::IndirectWorkload, {
        TaskLane::FrameCritical, TaskDomain::GraphPublication,
        "IndirectStateArtifact::Build", BuildIndirectState });
}

} // namespace br::render
