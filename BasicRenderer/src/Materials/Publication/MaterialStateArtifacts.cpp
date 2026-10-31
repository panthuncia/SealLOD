#include "Materials/Publication/MaterialStateArtifacts.h"

#include <BasicRenderer/Streaming/PublishedRendererState.h>
#include "Materials/TextureStreaming/TextureBindingArtifacts.h"
#include <BasicRenderer/Streaming/VersionedGpuBuffer.h>
#include "Resources/Buffers/Buffer.h"
#include "Resources/GloballyIndexedResource.h"
#include <BasicTelemetry/Telemetry.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

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

namespace {

// The binding requirements of a capture: the newest binding at or beyond each
// captured revision. Texture refresh can publish a successor before the batch is
// admitted, and an exact requirement would then wait on a reclaimed version.
std::vector<ArtifactRequirement> BindingRequirements(const MaterialUsageAdmission& admission) {
    std::vector<ArtifactRequirement> requirements;
    std::unordered_set<ArtifactAddress, ArtifactAddress::Hasher> selected;
    for (const auto& entry : admission.entries) {
        for (const auto& binding : entry.textureBindings) {
            const ArtifactAddress address{ ArtifactKind::TextureBinding, binding.streamingTextureID, 0 };
            if (!selected.insert(address).second) continue;
            requirements.push_back(LatestAtLeast(
                address, binding.bindingRevision, ArtifactReadiness::UploadSubmitted));
        }
    }
    return requirements;
}

// Whether the selected bindings still carry the descriptors the rows were captured with.
bool CaptureIsCurrent(const MaterialUsageAdmission& admission, const ArtifactBuildContext& context) {
    for (const auto& entry : admission.entries) {
        for (const auto& expected : entry.textureBindings) {
            const auto binding = context.Dependency<PublishedTextureBinding>({
                ArtifactKind::TextureBinding, expected.streamingTextureID, 0 });
            if (!binding || binding.revision < expected.bindingRevision ||
                binding.payload->imageDescriptorIndex != expected.imageDescriptorIndex ||
                binding.payload->samplerDescriptorIndex != expected.samplerDescriptorIndex) return false;
        }
    }
    return true;
}

} // namespace

void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::MaterialUsageBatch, {
        TaskLane::Streaming, TaskDomain::MaterialAcceptance,
        "MaterialStateArtifact::AdmitUsageBatch",
        [](const ArtifactBuildContext& context) {
            const auto input = context.input.Get<MaterialUsageBatchBuildInput>();
            if (!input || !input->capture) return ArtifactBuildResult::Failure(
                "material usage batch immutable input missing");
            if (context.stopRequested && context.stopRequested()) {
                return ArtifactBuildResult::Cancelled();
            }
            auto admission = context.checkpoint.Get<MaterialUsageAdmission>();
            if (admission && !CaptureIsCurrent(*admission, context)) {
                // A successor binding was published after the capture. Dropping the
                // capture rolls its reservation back; the next one observes the successor.
                basic_telemetry::AddCounter("SARP.Material.UsageBatchStaleTextureClosure");
                admission.reset();
            }
            if (!admission) {
                admission = input->capture();
                if (!admission || !admission->reservation || !admission->reservation->Result()) {
                    return ArtifactBuildResult::Failure("material usage batch reservation missing");
                }
                return ArtifactBuildResult::Needs(BindingRequirements(*admission),
                    ArtifactPayload::Make<MaterialUsageAdmission>(admission));
            }
            auto result = ArtifactBuildResult::Ready(
                ArtifactPayload::Make<PublishedMaterialUsageBatch>(
                    admission->reservation->Result()));
            const auto reservation = admission->reservation;
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
