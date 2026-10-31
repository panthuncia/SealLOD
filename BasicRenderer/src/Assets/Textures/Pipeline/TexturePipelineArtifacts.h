#pragma once

// The texture pipeline's graph jobs (see BasicRenderer/Assets/TexturePipeline.h).
//
//   TextureProcessed{cache key}   the processed texture: found in the processed cache, or (a
//                                 miss) decoded from its source and processed on the CPU or
//                                 by the GPU BC7 job. Shared by every texture with the key.
//   TextureCacheWrite{cache key}  writes a miss's result to the cache, in the background.
//                                 Nothing waits for it to show a texture.
//   TextureResidency{texture}     makes the mip window streaming wants resident: from the
//                                 cache file (DirectStorage, CPU fallback), or from a miss's
//                                 result before its cache file exists. One revision per window.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <BasicRenderer/Assets/TexturePipeline.h>

class TextureFactory;
namespace org { class PixelBuffer; }

namespace br::render {

class RendererStateRequestService;

// What every texture pipeline job uses; owned by the texture streaming manager.
struct TexturePipelineServices {
    std::shared_mutex mutex;
    const TextureFactory* factory = nullptr;  // null once the streaming manager shuts down
    std::function<void(std::uint64_t)> notifySuspension;
};

[[nodiscard]] std::uint64_t TextureCacheKeyHash(const std::string& cacheKey);

[[nodiscard]] inline ArtifactAddress TextureProcessedAddress(std::uint64_t cacheKeyHash) {
    return { ArtifactKind::TextureProcessed, cacheKeyHash, 0 };
}
[[nodiscard]] inline ArtifactAddress TextureCacheWriteAddress(std::uint64_t cacheKeyHash) {
    return { ArtifactKind::TextureCacheWrite, cacheKeyHash, 0 };
}
[[nodiscard]] inline ArtifactAddress TextureResidencyAddress(std::uint32_t streamingTextureID) {
    return { ArtifactKind::TextureResidency, streamingTextureID, 0 };
}

struct TextureProcessedInput {
    std::shared_ptr<const br::textures::TextureRequest> request;
    std::string cacheKey;
    std::shared_ptr<TexturePipelineServices> services;
};

// A miss's result, held until its cache file is written.
struct TextureProcessedResult {
    std::mutex mutex;
    std::shared_ptr<TextureSourceData> data;
    std::shared_ptr<org::PixelBuffer> gpuImage;  // the GPU BC7 job's image (full chain)
    std::wstring cachePath;                      // set once written
};

struct PublishedTextureProcessed {
    std::string cacheKey;
    org::TextureDescription fullDesc;  // every mip
    std::wstring cachePath;            // a hit's cache file
    std::shared_ptr<TextureProcessedResult> result;  // a miss's result
};

struct TextureCacheWriteInput {
    std::shared_ptr<const PublishedTextureProcessed> processed;
};

struct TextureResidencyInput {
    std::weak_ptr<TextureAsset> texture;
    std::uint64_t processedKeyHash = 0;
    std::shared_ptr<TexturePipelineServices> services;
};

struct PublishedTextureResidency {
    std::uint32_t streamingTextureID = 0;
    std::uint32_t topMip = 0;
    std::uint64_t bindingRevision = 0;
};

void RegisterTexturePipelineProducers(AsyncStateGraph& graph);

} // namespace br::render
