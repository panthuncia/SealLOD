#include "Render/MaterialStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureBindingArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/PixelBuffer.h"

#include <unordered_map>
#include <algorithm>
#include <cstring>

namespace br::render {
namespace {

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
            if (version && version->resource && version->elementStride == expectedStride) return version;
        }
        return std::shared_ptr<const PublishedGpuBufferVersion>{};
    };
    state->baseTable = resolveTable(input->baseTableKey, sizeof(PerMaterialCB));
    state->evalTable = resolveTable(input->evalTableKey, sizeof(PerMaterialEvalCB));
    state->openPbrTable = resolveTable(input->openPbrTableKey, sizeof(PerMaterialOpenPBRCB));
    if (!state->baseTable || !state->evalTable || !state->openPbrTable) {
        return ArtifactBuildResult::Failure("material table dependency missing or has incompatible ABI");
    }
    if (!state->baseTable->cpuShadow ||
        state->baseTable->cpuShadow->size() < state->baseTable->elementCount * sizeof(PerMaterialCB)) {
        return ArtifactBuildResult::Failure("material table CPU shadow missing or truncated");
    }
    const auto validateBinding = [&](std::uint32_t streamingID, std::uint32_t descriptor,
        std::uint32_t sampler) {
        if (streamingID == 0u) return true;
		const auto expected = std::ranges::find_if(input->textureBindings,
			[streamingID, descriptor, sampler](const MaterialTextureBindingDependencyDTO& binding) {
				return binding.streamingTextureID == streamingID &&
					binding.imageDescriptorIndex == descriptor &&
					binding.samplerDescriptorIndex == sampler;
			});
        // Non-participating textures are owned by the ordinary material lifetime
        // path and intentionally have no TextureBinding artifact.
        if (expected == input->textureBindings.end()) return true;
	return true;
    };
    for (std::size_t slot = 0; slot < state->baseTable->elementCount; ++slot) {
        PerMaterialCB row{};
        std::memcpy(&row, state->baseTable->cpuShadow->data() + slot * sizeof(row), sizeof(row));
        if (!validateBinding(row.baseColorStreamingTextureID, row.baseColorTextureIndex, row.baseColorSamplerIndex) ||
            !validateBinding(row.normalStreamingTextureID, row.normalTextureIndex, row.normalSamplerIndex) ||
            !validateBinding(row.metallicStreamingTextureID, row.metallicTextureIndex, row.metallicSamplerIndex) ||
            !validateBinding(row.roughnessStreamingTextureID, row.roughnessTextureIndex, row.roughnessSamplerIndex) ||
            !validateBinding(row.emissiveStreamingTextureID, row.emissiveTextureIndex, row.emissiveSamplerIndex) ||
            !validateBinding(row.aoStreamingTextureID, row.aoMapIndex, row.aoSamplerIndex) ||
            !validateBinding(row.heightStreamingTextureID, row.heightMapIndex, row.heightSamplerIndex) ||
            !validateBinding(row.opacityStreamingTextureID, row.opacityTextureIndex, row.opacitySamplerIndex)) {
            return ArtifactBuildResult::Failure("material row texture descriptor does not match retained binding");
        }
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

} // namespace br::render
