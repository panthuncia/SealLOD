#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "Render/AsyncStateGraph.h"
#include "Render/MaterialStateArtifacts.h"
#include "ShaderBuffers.h"

namespace org::runtime { class IUploadService; }

namespace br::render {

class RendererStateRequestService;
class VersionedBufferFamily;

inline constexpr std::uint64_t kTerrainSetsVariant = 101;
inline constexpr std::uint64_t kTerrainLayersVariant = 102;
inline constexpr std::uint64_t kTerrainStochasticLayersVariant = 103;
inline constexpr std::uint64_t kTerrainLayerRefsVariant = 104;
inline constexpr std::uint64_t kTerrainRegionsVariant = 105;
inline constexpr std::uint64_t kTerrainWeightBlocksVariant = 106;
inline constexpr std::uint64_t kTerrainTextureGroupVariant = 107;

enum class TerrainTextureTargetSlot : std::uint8_t {
    Diffuse,
    Normal,
    Height,
    Rmaos,
};

struct TerrainTextureTarget {
    ArtifactAddress bindingAddress{};
    std::uint32_t layerIndex = 0;
    TerrainTextureTargetSlot slot = TerrainTextureTargetSlot::Diffuse;
};

struct TerrainStateBuildInput {
    std::uint64_t terrainGeneration = 0;
    std::array<ArtifactVersionID, 6> bufferVersions{};
    std::vector<TerrainLayerGPU> baseLayers;
    std::vector<TerrainTextureTarget> textureTargets;
    RendererStateRequestService* requestService = nullptr;
    org::runtime::IUploadService* uploadService = nullptr;
    std::shared_ptr<VersionedBufferFamily> layerBufferFamily;
};

struct PublishedTerrainState {
    std::uint64_t terrainGeneration = 0;
    std::uint64_t stateRevision = 0;
    std::array<ArtifactSnapshot, 6> buffers{};
    std::vector<MaterialTextureBindingDependencyDTO> textureBindings;
};

void RegisterTerrainStateProducer(AsyncStateGraph& graph);

} // namespace br::render
