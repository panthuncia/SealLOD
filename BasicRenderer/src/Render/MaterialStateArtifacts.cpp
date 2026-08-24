#include "Render/MaterialStateArtifacts.h"

#include "Render/PublishedRendererState.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/TextureBindingArtifacts.h"
#include "Resources/GloballyIndexedResource.h"

#include <unordered_map>

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
	std::unordered_map<std::uint64_t, const ArtifactSnapshot*> textureDependencies;
	textureDependencies.reserve(input->textureBindings.size());
	for (const auto& dependency : context.dependencies) {
		if (dependency.key.kind == ArtifactKind::TextureBinding) {
			textureDependencies[dependency.key.primaryID] = &dependency;
		}
	}
	for (const auto& expected : input->textureBindings) {
		const auto found = textureDependencies.find(expected.streamingTextureID);
		if (found == textureDependencies.end() ||
			found->second->revision != expected.bindingRevision) {
			return ArtifactBuildResult::Failure("material texture-binding revision mismatch");
		}
		const auto binding = found->second->payload.Get<PublishedTextureBinding>();
		if (!binding || binding->bindingRevision != expected.bindingRevision ||
			binding->imageDescriptorIndex != expected.imageDescriptorIndex ||
			binding->samplerDescriptorIndex != expected.samplerDescriptorIndex) {
			return ArtifactBuildResult::Failure("material texture-binding descriptor mismatch");
		}
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
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Materials;
    root->fragment.revision = context.revision;
    // Material rows contain bindless descriptor indices. Retain the exact
    // texture-binding revisions that those indices were validated against for
    // as long as this published state (and any in-flight frame using it) lives.
    root->fragment.dependencyClosure = context.dependencies;
    for (const auto& expected : input->textureBindings) {
        const auto found = textureDependencies.find(expected.streamingTextureID);
        if (found == textureDependencies.end()) continue;
        const auto binding = found->second->payload.Get<PublishedTextureBinding>();
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
