#pragma once

#include <cstdint>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Render/PublishedRendererState.h"

namespace br::render {

inline constexpr std::uint64_t kObjectPerObjectVariant = 1;
inline constexpr std::uint64_t kObjectInstanceTransformVariant = 2;
inline constexpr std::uint64_t kObjectDrawRecordVariant = 3;
inline constexpr std::uint64_t kObjectNormalMatrixVariant = 4;
inline constexpr std::uint64_t kObjectVisibilityGenerationVariant = 5;

struct ObjectBufferDependencyDTO {
    ArtifactKey key{ ArtifactKind::BufferVersion, 0, 0 };
    std::uint64_t revision = 0;
    std::uint32_t elementStride = 0;
    std::uint64_t catalogVariant = 0;
};

struct ObjectBufferStateBuildInput {
    std::vector<ObjectBufferDependencyDTO> buffers;
};

struct PublishedObjectBufferState {
    std::vector<ObjectBufferDependencyDTO> buffers;
};

void RegisterObjectBufferStateProducer(AsyncStateGraph& graph);

} // namespace br::render
