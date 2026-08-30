#include "Render/MaterialStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include "Managers/MaterialManager.h"

#include <algorithm>

namespace br::render {
namespace {

ArtifactBuildResult BuildMaterialRow(const ArtifactBuildContext& context,
    MaterialManager& manager) {
    const auto input = context.input.Get<MaterialRowInput>();
    if (!input || input->materialID != context.key.primaryID ||
        input->sourceRevision != context.revision) {
        return ArtifactBuildResult::Failure("material-row immutable input identity mismatch");
    }
    auto row = std::make_shared<MaterialRowArtifact>();
    row->materialID = input->materialID;
    row->materialSlot = input->materialSlot;
    row->sourceRevision = input->sourceRevision;
    row->base = input->base;
    row->evaluation = input->evaluation;
    row->openPbr = input->openPbr;
    auto result = ArtifactBuildResult::Ready(
        ArtifactPayload::Make<MaterialRowArtifact>(row));
    result.acceptance = { TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        [&manager, row](const ArtifactSnapshot&) {
            (void)manager.ApplyMaterialRowArtifact(*row);
        } };
    return result;
}

ArtifactBuildResult BuildMaterialState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<MaterialStateBuildInput>();
    if (!input) return ArtifactBuildResult::Failure("material-state immutable input missing");

    auto state = std::make_shared<PublishedMaterialState>();
    state->sourceFingerprint = input->sourceFingerprint;
    state->compileFlagSlotsUsed = input->slotsUsed;
    state->activeCompileFlags.reserve(input->activeCompileFlags.size());
    state->activeCompileFlagSlots.reserve(input->activeCompileFlags.size());
    for (const auto& entry : input->activeCompileFlags) {
        if (entry.slot >= input->slotsUsed) continue;
        state->activeCompileFlags.push_back(entry.flags);
        state->activeCompileFlagSlots.push_back(entry.slot);
	}

    const auto resolveTable = [&](const ArtifactKey& key, std::uint32_t expectedStride) {
        for (const auto& dependency : context.dependencies) {
            if (dependency.key != key) continue;
            const auto fragment = dependency.payload.Get<RendererStateFragmentArtifact>();
            const auto version = fragment
                ? fragment->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
            if (version && version->resource && version->elementStride == expectedStride &&
                version->writeSequence == input->materialRowsRevision &&
                version->elementCount == input->materialRowCount) return version;
        }
        return std::shared_ptr<const PublishedGpuBufferVersion>{};
    };
    state->baseTable = resolveTable(input->baseTableKey, sizeof(PerMaterialCB));
    state->evalTable = resolveTable(input->evalTableKey, sizeof(PerMaterialEvalCB));
    state->openPbrTable = resolveTable(input->openPbrTableKey, sizeof(PerMaterialOpenPBRCB));
    if (!state->baseTable || !state->evalTable || !state->openPbrTable) {
        basic_telemetry::SetGauge("SARP.Material.RootRetry.ExpectedWriteSequence",
            static_cast<std::int64_t>(input->materialRowsRevision));
        basic_telemetry::SetGauge("SARP.Material.RootRetry.ExpectedRowCount",
            static_cast<std::int64_t>(input->materialRowCount));
        const auto reportCandidate = [&](const ArtifactKey& key, std::string_view table) {
            for (const auto& dependency : context.dependencies) {
                if (dependency.key != key) continue;
                const auto fragment = dependency.payload.Get<RendererStateFragmentArtifact>();
                const auto version = fragment
                    ? fragment->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
                if (!version) continue;
                basic_telemetry::SetGauge(std::string("SARP.Material.RootRetry.") +
                    std::string(table) + ".WriteSequence",
                    static_cast<std::int64_t>(version->writeSequence));
                basic_telemetry::SetGauge(std::string("SARP.Material.RootRetry.") +
                    std::string(table) + ".RowCount",
                    static_cast<std::int64_t>(version->elementCount));
            }
        };
        reportCandidate(input->baseTableKey, "Base");
        reportCandidate(input->evalTableKey, "Eval");
        reportCandidate(input->openPbrTableKey, "OpenPbr");
        basic_telemetry::AddCounter("SARP.Material.RootRetry.Count");
        // Dependencies are minimum-revision requirements. During rapid material
        // streaming an older root build can therefore be scheduled after its
        // table nodes have already advanced. Publishing that mixed closure maps
        // draw material slots to unrelated rows. Yield to the coalesced successor
        // rather than treating this expected race as a terminal graph failure.
        return ArtifactBuildResult::Retry(std::chrono::milliseconds(1));
    }
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Materials;
    root->fragment.revision = context.revision;
    root->fragment.dependencyClosure = context.dependencies;
    root->fragment.payload = ArtifactPayload::Make<PublishedMaterialState>(std::move(state));
    const auto addCatalogEntry = [&](std::uint64_t variant,
        const std::shared_ptr<const PublishedGpuBufferVersion>& version) {
        auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
        resources->push_back(version->resource);
        root->catalogEntries.emplace_back(PublishedResourceKey{
            PublishedFragmentKind::Materials, PublishedResourceUsage::ShaderResource,
            0, 0, variant }, std::move(resources));
    };
    const auto published = root->fragment.payload.Get<PublishedMaterialState>();
    addCatalogEntry(kMaterialBaseTableVariant, published->baseTable);
    addCatalogEntry(kMaterialEvalTableVariant, published->evalTable);
    addCatalogEntry(kMaterialOpenPbrTableVariant, published->openPbrTable);
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterMaterialStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::MaterialTable, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialStateArtifact::Build", BuildMaterialState });
}

void RegisterMaterialRowProducer(AsyncStateGraph& graph, MaterialManager& manager) {
    graph.RegisterProducer(ArtifactKind::Material, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialRowArtifact::Build",
        [&manager](const ArtifactBuildContext& context) {
            return BuildMaterialRow(context, manager);
        } });
}

void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph, MaterialManager& manager) {
    graph.RegisterProducer(ArtifactKind::MaterialUsageBatch, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialStateArtifact::AdmitUsageBatch",
        [&manager](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<MaterialUsageBatchBuildInput>();
            if (!input) return ArtifactBuildResult::Failure(
                "material usage batch immutable input missing");
            if (context.stopRequested && context.stopRequested()) {
                return ArtifactBuildResult::Cancelled();
            }
            auto result = manager.ApplyMaterialUsageBatch(*input);
            return result
                ? ArtifactBuildResult::Ready(
                    ArtifactPayload::Make<PublishedMaterialUsageBatch>(std::move(result)))
                : ArtifactBuildResult::Failure("material usage batch admission failed");
        }
    });
}

} // namespace br::render
