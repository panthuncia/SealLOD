#include "Render/MaterialStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/TextureBindingArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <stdexcept>

namespace br::render {
namespace {

ArtifactBuildResult BuildMaterialRow(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<MaterialRowInput>();
    if (!input || input->materialID != context.key.primaryID ||
        input->sourceRevision != context.revision) {
        return ArtifactBuildResult::Failure("material-row immutable input identity mismatch");
    }
    const auto row = input->reservation ? input->reservation->Row() : nullptr;
    if (!row || row->materialID != input->materialID ||
        row->materialSlot != input->materialSlot ||
        row->sourceRevision != input->sourceRevision) {
        return ArtifactBuildResult::Failure("material-row reservation identity mismatch");
    }
    auto result = ArtifactBuildResult::Ready(
        ArtifactPayload::Make<MaterialRowArtifact>(row));
    const auto reservation = input->reservation;
    result.acceptance = { TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        [reservation](const ArtifactSnapshot&) {
            if (!reservation->Commit()) {
                throw std::runtime_error("material-row reservation commit failed");
            }
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
    state->rasterBucketFlags = input->rasterBucketFlags;
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

void RegisterMaterialRowProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::Material, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialRowArtifact::Build",
        BuildMaterialRow });
}

void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::MaterialUsageBatch, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialStateArtifact::AdmitUsageBatch",
        [](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<MaterialUsageBatchBuildInput>();
            if (!input) return ArtifactBuildResult::Failure(
                "material usage batch immutable input missing");
            if (context.stopRequested && context.stopRequested()) {
                return ArtifactBuildResult::Cancelled();
            }
            for (const auto& entry : input->entries) {
                for (const auto& expected : entry.textureBindings) {
                    const auto binding = context.Dependency<PublishedTextureBinding>({
                        ArtifactKind::TextureBinding, expected.streamingTextureID, 0 });
                    if (!binding || binding.revision < expected.bindingRevision ||
                        binding.payload->imageDescriptorIndex != expected.imageDescriptorIndex ||
                        binding.payload->samplerDescriptorIndex != expected.samplerDescriptorIndex) {
                        // LatestAtLeast may legitimately select a texture successor
                        // published after this immutable material row was captured. Its
                        // descriptor indices then differ even though neither asset has
                        // failed. Cancel this stale capture so the owner can roll back its
                        // reservation and recapture against the successor binding.
                        basic_telemetry::AddCounter(
                            "SARP.Material.UsageBatchStaleTextureClosure");
                        return ArtifactBuildResult::Cancelled();
                    }
                }
            }
            if (!input->reservation || !input->reservation->Result()) {
                return ArtifactBuildResult::Failure("material usage batch reservation missing");
            }
            auto result = ArtifactBuildResult::Ready(
                ArtifactPayload::Make<PublishedMaterialUsageBatch>(
                    input->reservation->Result()));
            const auto reservation = input->reservation;
            result.acceptance = { TaskLane::Streaming, TaskDomain::MaterialAcceptance,
                [reservation](const ArtifactSnapshot&) {
                    if (!reservation->Commit()) {
                        throw std::runtime_error("material usage reservation commit failed");
                    }
                } };
            return result;
        }
    });
}

} // namespace br::render
