#pragma once

#include "ProceduralWind/WindTypes.h"

namespace br::wind {

WindBakeResult BakeLevel0WindField(
    WindTerrainMeshView terrain,
    std::uint64_t sourceHash,
    const WindBakeSettings& settings = {});

bool SaveWindCache(
    const std::filesystem::path& path,
    const WindBakeResult& bake,
    std::string* error = nullptr);

bool LoadWindCacheMetadata(
    const std::filesystem::path& path,
    WindCacheMetadata& metadata,
    std::string* error = nullptr);

bool LoadWindDirection(
    const std::filesystem::path& path,
    const WindCacheMetadata& metadata,
    std::uint32_t directionIndex,
    std::vector<std::uint16_t>& rgba16f,
    std::string* error = nullptr);

bool IsWindCacheCurrent(
    const std::filesystem::path& path,
    std::uint64_t sourceHash,
    const WindBakeSettings& settings);

} // namespace br::wind
