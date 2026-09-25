#pragma once

#include <cstdint>
#include <memory>

#include "Render/AsyncStateGraph.h"
#include "ShaderBuffers.h"

namespace org { class PixelBuffer; }

namespace br::render {

struct TextureTransferArtifact;

struct TextureBindingBuildInput {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint64_t streamingStateRevision = 0;
    std::uint32_t samplerDescriptorIndex = 0;
    std::shared_ptr<org::PixelBuffer> image;
	std::shared_ptr<const TextureTransferArtifact> transfer;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
    TextureStreamingGPUInfo streamingMetadata{};
};

struct PublishedTextureBinding {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint64_t streamingStateRevision = 0;
    std::uint32_t imageDescriptorIndex = 0;
    std::uint32_t samplerDescriptorIndex = 0;
    std::shared_ptr<org::PixelBuffer> image;
	std::shared_ptr<const TextureTransferArtifact> transfer;
    TextureStreamingGPUInfo streamingMetadata{};
};

// Display readiness of one streaming texture, latched once per quality level.
// Renderables require the gates of every texture they sample, so they become
// visible only once the published image table binds a usable image for each of
// them (not the shared processing placeholder, and for alpha-tested use not a
// mip too coarse to preserve coverage), or once the texture has explicitly
// failed to load and the placeholder is final.
enum class TextureDisplayQuality : std::uint64_t {
    AnyImage = 0,
    AlphaCoverage = 1,
};

[[nodiscard]] inline ArtifactAddress TextureDisplayGateAddress(
    std::uint32_t streamingTextureID, TextureDisplayQuality quality) {
    return { ArtifactKind::TextureDisplayGate, streamingTextureID,
        static_cast<std::uint64_t>(quality) };
}

struct TextureDisplayGateInput {
    std::uint32_t streamingTextureID = 0;
    TextureDisplayQuality quality = TextureDisplayQuality::AnyImage;
    std::uint64_t bindingRevision = 0;
    bool loadFailed = false;
};
using PublishedTextureDisplayGate = TextureDisplayGateInput;

void RegisterTextureBindingProducer(AsyncStateGraph& graph);
void RegisterTextureDisplayGateProducer(AsyncStateGraph& graph);

} // namespace br::render
