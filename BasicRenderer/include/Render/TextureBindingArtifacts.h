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

void RegisterTextureBindingProducer(AsyncStateGraph& graph);

} // namespace br::render
