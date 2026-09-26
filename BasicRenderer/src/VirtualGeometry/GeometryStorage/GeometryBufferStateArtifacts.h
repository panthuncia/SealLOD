#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Runtime/StateGraph/AsyncStateGraph.h"
#include <BasicRenderer/Streaming/PublishedRendererState.h>

namespace br::render {

struct PublishedGpuBufferVersion;

struct GeometryBufferDependencyDTO {
    ArtifactKey key{ ArtifactKind::BufferVersion, 0, 0 };
    std::uint64_t revision = 0;
    std::uint32_t elementStride = 0;
    std::uint64_t catalogVariant = 0;
};

struct GeometryBufferStateBuildInput {
    std::vector<GeometryBufferDependencyDTO> buffers;
    // MeshManager journal mutation sequence contained in this cut.
    std::uint64_t coveredMutationSequence = 0;
};

struct PublishedGeometryBufferState {
    std::vector<GeometryBufferDependencyDTO> buffers;
    std::vector<std::shared_ptr<const PublishedGpuBufferVersion>> versions;
};

void RegisterGeometryBufferStateProducer(AsyncStateGraph& graph);

} // namespace br::render
