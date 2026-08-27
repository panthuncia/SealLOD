#pragma once

#include <cstdint>
#include <memory>

#include "Render/AsyncStateGraph.h"

namespace org { class PixelBuffer; }

namespace br::render {

struct TextureBindingBuildInput {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint64_t streamingStateRevision = 0;
    std::uint32_t samplerDescriptorIndex = 0;
    std::shared_ptr<org::PixelBuffer> image;
    std::shared_ptr<const GpuSubmissionSet> gpuSubmissions;
};

struct PublishedTextureBinding {
    std::uint32_t streamingTextureID = 0;
    std::uint64_t bindingRevision = 0;
    std::uint64_t streamingStateRevision = 0;
    std::uint32_t imageDescriptorIndex = 0;
    std::uint32_t samplerDescriptorIndex = 0;
    std::shared_ptr<org::PixelBuffer> image;
};

void RegisterTextureBindingProducer(AsyncStateGraph& graph);

} // namespace br::render
