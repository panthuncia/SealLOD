#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace br::wind {

inline constexpr std::uint32_t kWindCacheMagic = 0x444E4957u; // WIND
inline constexpr std::uint32_t kWindCacheSchemaVersion = 1u;
inline constexpr std::uint32_t kInvalidSimulationGroup = 0xFFFFFFFFu;

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct WindTerrainMeshView {
    std::span<const Float3> positions;
    std::span<const std::uint32_t> indices;
};

struct WindBakeSettings {
    float cellSize = 4096.0f;
    std::uint32_t directionCount = 16u;
    float atmosphericClearanceCells = 4.0f;
    float minimumLayerDepthCells = 1.0f;
    float maximumUpliftRatio = 0.5f;
    std::uint32_t maximumIterations = 1000u;
    double relativeTolerance = 1.0e-6;
};

struct WindDirectionBlock {
    float angleRadians = 0.0f;
    std::uint64_t fileOffset = 0u;
    std::uint32_t compressedBytes = 0u;
    std::uint32_t uncompressedBytes = 0u;
    std::uint64_t checksum = 0u;
};

struct WindCacheMetadata {
    std::uint32_t schemaVersion = kWindCacheSchemaVersion;
    std::uint64_t sourceHash = 0u;
    std::uint64_t configHash = 0u;
    Float3 origin{};
    float cellSize = 0.0f;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t layerCount = 1u;
    std::vector<WindDirectionBlock> directions;
};

struct WindBakeResult {
    WindCacheMetadata metadata;
    std::vector<std::vector<std::uint16_t>> directionRgba16f;
    std::vector<std::uint8_t> coverage;
    bool converged = false;
    std::string error;
};

struct WindState {
    Float3 directionToWS{ 1.0f, 0.0f, 0.0f };
    float strength = 0.0f;
    float gustStrength = 0.0f;
};

struct WindDirectionBracket {
    std::uint32_t lower = 0u;
    std::uint32_t upper = 0u;
    float interpolation = 0.0f;
};

WindDirectionBracket ComputeDirectionBracket(float angleRadians, std::uint32_t directionCount);
std::uint64_t ComputeWindBakeConfigHash(const WindBakeSettings& settings);

} // namespace br::wind
