#include "Render/TerrainStateArtifacts.h"

#include <algorithm>
#include <unordered_map>

#include "Render/PublishedRendererState.h"
#include "Render/TextureBindingArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildTerrainState(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TerrainStateBuildInput>();
    if (!input || input->terrainGeneration != context.key.variantID ||
        input->stateRevision == 0 || input->stateRevision != context.revision) {
        return ArtifactBuildResult::Failure("terrain-state generation mismatch");
    }
    constexpr std::array<std::uint32_t, 6> expectedStrides{
        sizeof(TerrainSetGPU), sizeof(TerrainLayerGPU), sizeof(TerrainStochasticLayerGPU),
        sizeof(TerrainLayerRefGPU), sizeof(TerrainRegionGPU), sizeof(std::uint32_t) };
    auto state = std::make_shared<PublishedTerrainState>();
    state->terrainGeneration = input->terrainGeneration;
    state->stateRevision = input->stateRevision;
    state->textureBindings = input->textureBindings;
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Terrain;
    root->fragment.revision = context.revision;
    root->fragment.dependencyClosure = context.dependencies;
    for (std::size_t index = 0; index < input->bufferKeys.size(); ++index) {
        const auto found = std::ranges::find_if(context.dependencies,
            [&](const ArtifactSnapshot& dependency) { return dependency.key == input->bufferKeys[index]; });
        const auto fragment = found != context.dependencies.end()
            ? found->payload.Get<RendererStateFragmentArtifact>() : nullptr;
        const auto version = fragment
            ? fragment->fragment.payload.Get<PublishedGpuBufferVersion>() : nullptr;
        if (!version || !version->resource || version->elementStride != expectedStrides[index]) {
            return ArtifactBuildResult::Failure("terrain buffer dependency missing or incompatible");
        }
        state->buffers[index] = *found;
        auto resources = std::make_shared<PublishedResourceCatalog::ResourceList>();
        resources->push_back(version->resource);
        root->catalogEntries.emplace_back(PublishedResourceKey{
            PublishedFragmentKind::Terrain, PublishedResourceUsage::ShaderResource,
            0, 0, input->bufferKeys[index].variantID }, std::move(resources));
    }
    std::unordered_map<std::uint64_t, const ArtifactSnapshot*> textureDependencies;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind == ArtifactKind::TextureBinding) {
            textureDependencies[dependency.key.primaryID] = &dependency;
        }
    }
    auto textureResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    for (const auto& expected : input->textureBindings) {
        const auto found = textureDependencies.find(expected.streamingTextureID);
        const auto binding = found != textureDependencies.end()
            ? found->second->payload.Get<PublishedTextureBinding>() : nullptr;
        if (!binding || found->second->revision != expected.bindingRevision ||
            binding->imageDescriptorIndex != expected.imageDescriptorIndex ||
            binding->samplerDescriptorIndex != expected.samplerDescriptorIndex) {
            return ArtifactBuildResult::Failure("terrain texture-binding closure mismatch");
        }
        root->fragment.resourceHolds.push_back(binding->image);
        if (std::ranges::find(*textureResources, binding->image) == textureResources->end()) {
            textureResources->push_back(binding->image);
        }
    }
    root->catalogEntries.emplace_back(PublishedResourceKey{
        PublishedFragmentKind::Terrain, PublishedResourceUsage::ShaderResource,
        0, 0, kTerrainTextureGroupVariant }, std::move(textureResources));
    root->fragment.payload = ArtifactPayload::Make<PublishedTerrainState>(std::move(state));
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<RendererStateFragmentArtifact>(std::move(root)));
}

} // namespace

void RegisterTerrainStateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::TerrainState, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "TerrainStateArtifact::Build", BuildTerrainState });
}

} // namespace br::render
