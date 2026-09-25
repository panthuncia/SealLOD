#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "ShaderBuffers.h"

namespace org { class PixelBuffer; }

namespace br::render {

class VersionedBufferFamily;
struct PublishedGpuBufferVersion;

inline constexpr std::uint64_t kTextureImageTableBufferVariant = 0x54494d47ull;
inline constexpr std::size_t kTextureImageHoldChunkSize = 64;

// Holds are chunked so an image successor clones one small ownership block.
// Published epochs share all unaffected chunks with their predecessor.
struct TextureImageHoldChunk {
    std::array<std::shared_ptr<org::PixelBuffer>, kTextureImageHoldChunkSize> images{};
};

struct TextureImageTableBuildInput {
    std::uint64_t contentEpoch = 0;
    std::uint64_t logicalExtent = 0;
    ArtifactKey bufferKey{ ArtifactKind::BufferVersion, 0, kTextureImageTableBufferVariant };
    std::shared_ptr<VersionedBufferFamily> bufferFamily;
    std::vector<std::shared_ptr<const TextureImageHoldChunk>> holdChunks;
};

struct PublishedTextureImageTable {
    std::uint64_t bindingEpoch = 0;
    std::uint64_t contentEpoch = 0;
    std::uint64_t logicalExtent = 0;
    std::shared_ptr<const PublishedGpuBufferVersion> table;
    std::vector<std::shared_ptr<const TextureImageHoldChunk>> holdChunks;
};

void RegisterTextureImageTableProducer(AsyncStateGraph& graph);

} // namespace br::render
