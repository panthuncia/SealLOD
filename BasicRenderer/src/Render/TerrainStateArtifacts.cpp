#include "Render/TerrainStateArtifacts.h"

#include <algorithm>
#include <unordered_map>

#include "Render/PublishedRendererState.h"
#include "Render/RendererStateRequestService.h"
#include "Render/TextureBindingArtifacts.h"
#include "Render/VersionedGpuBufferArtifacts.h"
#include "Render/Runtime/IUploadService.h"
#include "Resources/GloballyIndexedResource.h"
#include "Resources/PixelBuffer.h"
#include "ShaderBuffers.h"
#include <BasicTelemetry/Telemetry.h>

namespace br::render {
namespace {

ArtifactBuildResult BuildTerrainState(const ArtifactBuildContext& context) {
    basic_telemetry::AddCounter("SARP.Terrain.BuildAttempts");
    const auto input = context.input.Get<TerrainStateBuildInput>();
    if (!input || input->terrainGeneration != context.key.variantID ||
        !input->requestService || !input->uploadService || !input->layerBufferFamily ||
        context.revision == 0) {
        return ArtifactBuildResult::Failure("terrain-state generation mismatch");
    }
    auto layers = input->baseLayers;
    std::unordered_map<std::uint64_t, const ArtifactSnapshot*> textureDependencies;
    for (const auto& dependency : context.dependencies) {
        if (dependency.key.kind == ArtifactKind::TextureBinding) {
            textureDependencies[dependency.key.primaryID] = &dependency;
        }
    }
    std::unordered_map<std::uint64_t, MaterialTextureBindingDependencyDTO> selectedBindings;
    for (const auto& target : input->textureTargets) {
        const auto found = textureDependencies.find(target.bindingAddress.primaryID);
        const auto binding = found != textureDependencies.end()
            ? found->second->payload.Get<PublishedTextureBinding>() : nullptr;
        if (!binding || !binding->image || target.layerIndex >= layers.size()) continue;
        auto& layer = layers[target.layerIndex];
        switch (target.slot) {
        case TerrainTextureTargetSlot::Diffuse:
            layer.diffuseTextureIndex = binding->imageDescriptorIndex;
            layer.diffuseSamplerIndex = binding->samplerDescriptorIndex;
            layer.diffuseStreamingTextureID = binding->streamingTextureID;
            break;
        case TerrainTextureTargetSlot::Normal:
            layer.normalTextureIndex = binding->imageDescriptorIndex;
            layer.normalSamplerIndex = binding->samplerDescriptorIndex;
            layer.normalStreamingTextureID = binding->streamingTextureID;
            break;
        case TerrainTextureTargetSlot::Height:
            layer.heightTextureIndex = binding->imageDescriptorIndex;
            layer.heightSamplerIndex = binding->samplerDescriptorIndex;
            layer.heightStreamingTextureID = binding->streamingTextureID;
            break;
        case TerrainTextureTargetSlot::Rmaos:
            layer.rmaosTextureIndex = binding->imageDescriptorIndex;
            layer.rmaosSamplerIndex = binding->samplerDescriptorIndex;
            layer.rmaosStreamingTextureID = binding->streamingTextureID;
            break;
        }
        selectedBindings[binding->streamingTextureID] = {
            binding->streamingTextureID, binding->bindingRevision,
            binding->imageDescriptorIndex, binding->samplerDescriptorIndex };
    }
    // Latest texture edges can rebuild this immutable terrain recipe without
    // changing the source terrain revision.  The derived layer buffer must
    // therefore have its own monotonic content revision rather than reusing
    // context.revision for different bytes.
    const auto layerRequest = input->layerBufferFamily->RequestContentSnapshot(
        *input->requestService, *input->uploadService,
        std::as_bytes(std::span(layers)), layers.size());
    if (!layerRequest) {
        return ArtifactBuildResult::Failure("terrain derived layer-buffer request rejected");
    }
    auto bufferVersions = input->bufferVersions;
    bufferVersions[1] = layerRequest.version;
    const auto layerDependency = std::ranges::find_if(context.dependencies,
        [&](const ArtifactSnapshot& dependency) {
            return dependency.Version() == layerRequest.version &&
                ArtifactReachedMilestone(dependency.readiness,
                    ArtifactReadiness::UploadSubmitted);
        });
    if (layerDependency == context.dependencies.end()) {
        std::vector<ArtifactRequirement> requirements;
        requirements.reserve(bufferVersions.size() + input->textureTargets.size());
        for (const auto version : bufferVersions) {
            requirements.push_back(Exact(version, ArtifactReadiness::UploadSubmitted));
        }
        // Freeze the dependency selection for this immutable terrain version.
        // The graph retains the original Latest recipe separately and creates a
        // successor if another binding advances while this upload is pending.
        for (const auto& [_, dependency] : textureDependencies) {
            requirements.push_back(Exact(dependency->Version(),
                ArtifactReadiness::UploadSubmitted));
        }
        return ArtifactBuildResult::Needs(std::move(requirements));
    }

    constexpr std::array<std::uint32_t, 6> expectedStrides{
        sizeof(TerrainSetGPU), sizeof(TerrainLayerGPU), sizeof(TerrainStochasticLayerGPU),
        sizeof(TerrainLayerRefGPU), sizeof(TerrainRegionGPU), sizeof(std::uint32_t) };
    auto state = std::make_shared<PublishedTerrainState>();
    state->terrainGeneration = input->terrainGeneration;
    state->stateRevision = context.revision;
    state->textureBindings.reserve(selectedBindings.size());
    for (const auto& [_, binding] : selectedBindings) state->textureBindings.push_back(binding);
    auto root = std::make_shared<RendererStateFragmentArtifact>();
    root->kind = PublishedFragmentKind::Terrain;
    root->fragment.revision = context.revision;
    root->fragment.dependencyClosure = context.dependencies;
    for (std::size_t index = 0; index < bufferVersions.size(); ++index) {
        const auto found = std::ranges::find_if(context.dependencies,
            [&](const ArtifactSnapshot& dependency) {
                return dependency.Version() == bufferVersions[index];
            });
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
            0, 0, bufferVersions[index].address.variantID }, std::move(resources));
    }
    auto textureResources = std::make_shared<PublishedResourceCatalog::ResourceList>();
    for (const auto& expected : state->textureBindings) {
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
