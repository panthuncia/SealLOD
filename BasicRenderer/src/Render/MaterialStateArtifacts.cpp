#include "Render/MaterialStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureBindingArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/PixelBuffer.h"
#include "Managers/MaterialManager.h"

#include <unordered_map>

namespace br::render {
namespace {

void ApplyBinding(MaterialRowArtifact& row, MaterialTextureTarget target,
    const PublishedTextureBinding& binding) {
    const auto image = binding.imageDescriptorIndex;
    const auto sampler = binding.samplerDescriptorIndex;
    const auto streamingID = binding.streamingTextureID;
    auto patchBase = [&](std::uint32_t& texture, std::uint32_t& samplerField,
        std::uint32_t& streaming) {
        texture = image; samplerField = sampler; streaming = streamingID;
    };
    auto patchEval = [&](std::uint32_t& texture, std::uint32_t& samplerField,
        std::uint32_t& streaming) {
        texture = image; samplerField = sampler; streaming = streamingID;
    };
    switch (target) {
    case MaterialTextureTarget::BaseColor:
        patchBase(row.base.baseColorTextureIndex, row.base.baseColorSamplerIndex,
            row.base.baseColorStreamingTextureID);
        patchEval(row.evaluation.baseColorTextureIndex, row.evaluation.baseColorSamplerIndex,
            row.evaluation.baseColorStreamingTextureID); break;
    case MaterialTextureTarget::Normal:
        patchBase(row.base.normalTextureIndex, row.base.normalSamplerIndex,
            row.base.normalStreamingTextureID);
        patchEval(row.evaluation.normalTextureIndex, row.evaluation.normalSamplerIndex,
            row.evaluation.normalStreamingTextureID); break;
    case MaterialTextureTarget::Metallic:
        patchBase(row.base.metallicTextureIndex, row.base.metallicSamplerIndex,
            row.base.metallicStreamingTextureID);
        patchEval(row.evaluation.metallicTextureIndex, row.evaluation.metallicSamplerIndex,
            row.evaluation.metallicStreamingTextureID); break;
    case MaterialTextureTarget::Roughness:
        patchBase(row.base.roughnessTextureIndex, row.base.roughnessSamplerIndex,
            row.base.roughnessStreamingTextureID);
        patchEval(row.evaluation.roughnessTextureIndex, row.evaluation.roughnessSamplerIndex,
            row.evaluation.roughnessStreamingTextureID); break;
    case MaterialTextureTarget::Emissive:
        patchBase(row.base.emissiveTextureIndex, row.base.emissiveSamplerIndex,
            row.base.emissiveStreamingTextureID);
        patchEval(row.evaluation.emissiveTextureIndex, row.evaluation.emissiveSamplerIndex,
            row.evaluation.emissiveStreamingTextureID); break;
    case MaterialTextureTarget::AmbientOcclusion:
        patchBase(row.base.aoMapIndex, row.base.aoSamplerIndex, row.base.aoStreamingTextureID);
        patchEval(row.evaluation.aoMapIndex, row.evaluation.aoSamplerIndex,
            row.evaluation.aoStreamingTextureID); break;
    case MaterialTextureTarget::Height:
        patchBase(row.base.heightMapIndex, row.base.heightSamplerIndex,
            row.base.heightStreamingTextureID);
        patchEval(row.evaluation.heightMapIndex, row.evaluation.heightSamplerIndex,
            row.evaluation.heightStreamingTextureID); break;
    case MaterialTextureTarget::Opacity:
        patchBase(row.base.opacityTextureIndex, row.base.opacitySamplerIndex,
            row.base.opacityStreamingTextureID);
        patchEval(row.evaluation.opacityTextureIndex, row.evaluation.opacitySamplerIndex,
            row.evaluation.opacityStreamingTextureID); break;
    case MaterialTextureTarget::CoatColor:
        row.openPbr.coatColorTextureIndex = image; row.openPbr.coatColorSamplerIndex = sampler;
        row.openPbr.coatColorStreamingTextureID = streamingID; break;
    case MaterialTextureTarget::CoatWeight:
        row.openPbr.coatWeightTextureIndex = image; row.openPbr.coatWeightSamplerIndex = sampler;
        row.openPbr.coatWeightStreamingTextureID = streamingID; break;
    case MaterialTextureTarget::CoatRoughness:
        row.openPbr.coatRoughnessTextureIndex = image; row.openPbr.coatRoughnessSamplerIndex = sampler;
        row.openPbr.coatRoughnessStreamingTextureID = streamingID; break;
    case MaterialTextureTarget::FuzzColor:
        row.openPbr.fuzzColorTextureIndex = image; row.openPbr.fuzzColorSamplerIndex = sampler;
        row.openPbr.fuzzColorStreamingTextureID = streamingID; break;
    case MaterialTextureTarget::FuzzWeight:
        row.openPbr.fuzzWeightTextureIndex = image; row.openPbr.fuzzWeightSamplerIndex = sampler;
        row.openPbr.fuzzWeightStreamingTextureID = streamingID; break;
    case MaterialTextureTarget::FuzzRoughness:
        row.openPbr.fuzzRoughnessTextureIndex = image; row.openPbr.fuzzRoughnessSamplerIndex = sampler;
        row.openPbr.fuzzRoughnessStreamingTextureID = streamingID; break;
    }
}

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
    std::unordered_map<ArtifactAddress, std::shared_ptr<const PublishedTextureBinding>,
        ArtifactAddress::Hasher> bindings;
    for (const auto& dependency : context.dependencies) {
        if (auto binding = dependency.payload.Get<PublishedTextureBinding>()) {
            bindings.insert_or_assign(dependency.key, binding);
            row->textureBindings.push_back(dependency.Version());
            row->selectedBindings.push_back(std::move(binding));
        }
    }
    for (const auto& target : input->textureTargets) {
        if (const auto found = bindings.find(target.bindingAddress); found != bindings.end()) {
            ApplyBinding(*row, target.target, *found->second);
        }
    }
    if (!manager.ApplyMaterialRowArtifact(*row)) {
        return ArtifactBuildResult::Cancelled();
    }
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<MaterialRowArtifact>(std::move(row)));
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
	for (const auto& binding : input->preparedTextureBindings) {
		if (!binding || !binding->image || !binding->image->HasValidBackingResource()) {
			return ArtifactBuildResult::Failure("prepared material texture binding is invalid");
		}
		state->textureBindings.push_back(binding);
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
    // Material rows contain bindless descriptor indices. Retain the exact
    // texture-binding revisions that those indices were validated against for
    // as long as this published state (and any in-flight frame using it) lives.
    root->fragment.dependencyClosure = context.dependencies;
	for (const auto& binding : input->preparedTextureBindings) {
		if (binding && binding->image) root->fragment.resourceHolds.push_back(binding->image);
	}
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
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "MaterialStateArtifact::Build", BuildMaterialState });
}

void RegisterMaterialRowProducer(AsyncStateGraph& graph, MaterialManager& manager) {
    graph.RegisterProducer(ArtifactKind::Material, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "MaterialRowArtifact::Build",
        [&manager](const ArtifactBuildContext& context) {
            return BuildMaterialRow(context, manager);
        } });
}

void RegisterMaterialUsageBatchProducer(AsyncStateGraph& graph, MaterialManager& manager) {
    graph.RegisterProducer(ArtifactKind::MaterialUsageBatch, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
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
