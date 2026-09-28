#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "Runtime/StateGraph/AsyncStateGraph.h"
#include "BasicRenderer/Extensions/ShaderBuffers.h"

namespace org { class BindingTableVersion; }

namespace br::render {

class VersionedBufferFamily;
struct PublishedGpuBufferVersion;

inline constexpr std::uint64_t kTextureImageTableBufferVariant = 0x54494d47ull;
struct TextureImageTableBuildInput {
    std::uint64_t contentEpoch = 0;
    std::uint64_t logicalExtent = 0;
    ArtifactKey bufferKey{ ArtifactKind::BufferVersion, 0, kTextureImageTableBufferVariant };
    std::shared_ptr<VersionedBufferFamily> bufferFamily;
    std::shared_ptr<const org::BindingTableVersion> bindings;
};

struct PublishedTextureImageTable {
    std::uint64_t bindingEpoch = 0;
    std::uint64_t contentEpoch = 0;
    std::uint64_t logicalExtent = 0;
    std::shared_ptr<const PublishedGpuBufferVersion> table;
    std::shared_ptr<const org::BindingTableVersion> bindings;
};

void RegisterTextureImageTableProducer(AsyncStateGraph& graph);

} // namespace br::render
