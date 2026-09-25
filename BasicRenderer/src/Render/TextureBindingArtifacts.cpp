#include "Render/TextureBindingArtifacts.h"

#include "Resources/Texture.h"
#include "Managers/MaterialTextureTransferService.h"

namespace br::render {
namespace {

ArtifactBuildResult BuildTextureBinding(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureBindingBuildInput>();
    if (!input || input->streamingTextureID == 0 ||
        input->streamingTextureID != context.key.primaryID ||
        context.key.variantID != 0 ||
        input->bindingRevision == 0 || input->bindingRevision != context.revision) {
        return ArtifactBuildResult::Failure("texture binding identity or revision mismatch");
    }
    if (!input->image || !input->image->HasValidBackingResource()) {
        return ArtifactBuildResult::Failure("texture binding image is not GPU-ready");
    }
    const auto& srv = input->image->GetSRVInfo(0);
    auto binding = std::make_shared<PublishedTextureBinding>();
    binding->streamingTextureID = input->streamingTextureID;
    binding->bindingRevision = input->bindingRevision;
    binding->streamingStateRevision = input->streamingStateRevision;
    binding->imageDescriptorIndex = srv.slot.index;
    binding->samplerDescriptorIndex = input->samplerDescriptorIndex;
    binding->image = input->image;
	binding->transfer = input->transfer;
    binding->streamingMetadata = input->streamingMetadata;
    return ArtifactBuildResult::Ready(
        ArtifactPayload::Make<PublishedTextureBinding>(std::move(binding)),
		input->transfer ? input->transfer->gpuSubmissions : input->gpuSubmissions);
}

ArtifactBuildResult BuildTextureDisplayGate(const ArtifactBuildContext& context) {
    const auto input = context.input.Get<TextureDisplayGateInput>();
    if (!input || context.key != TextureDisplayGateAddress(input->streamingTextureID, input->quality)) {
        return ArtifactBuildResult::Failure("texture display gate identity mismatch");
    }
    return ArtifactBuildResult::Ready(ArtifactPayload::Make<PublishedTextureDisplayGate>(input));
}

} // namespace

void RegisterTextureDisplayGateProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::TextureDisplayGate, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "TextureDisplayGate::Build", BuildTextureDisplayGate });
}

void RegisterTextureBindingProducer(AsyncStateGraph& graph) {
    graph.RegisterProducer(ArtifactKind::TextureBinding, {
        TaskLane::Streaming, TaskDomain::TextureProcessing,
        "TextureBindingArtifact::Build", BuildTextureBinding });
}

} // namespace br::render
