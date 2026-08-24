#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Render/MaterialStateArtifacts.h"

namespace br::render {

inline constexpr std::uint64_t kTerrainSetsVariant = 101;
inline constexpr std::uint64_t kTerrainLayersVariant = 102;
inline constexpr std::uint64_t kTerrainStochasticLayersVariant = 103;
inline constexpr std::uint64_t kTerrainLayerRefsVariant = 104;
inline constexpr std::uint64_t kTerrainRegionsVariant = 105;
inline constexpr std::uint64_t kTerrainWeightBlocksVariant = 106;
inline constexpr std::uint64_t kTerrainTextureGroupVariant = 107;

struct TerrainStateBuildInput {
    std::uint64_t terrainGeneration = 0;
    std::uint64_t stateRevision = 0;
    std::array<ArtifactKey, 6> bufferKeys{};
    std::vector<MaterialTextureBindingDependencyDTO> textureBindings;
};

struct PublishedTerrainState {
    std::uint64_t terrainGeneration = 0;
    std::uint64_t stateRevision = 0;
    std::array<ArtifactSnapshot, 6> buffers{};
    std::vector<MaterialTextureBindingDependencyDTO> textureBindings;
};

void RegisterTerrainStateProducer(AsyncStateGraph& graph);

} // namespace br::render
