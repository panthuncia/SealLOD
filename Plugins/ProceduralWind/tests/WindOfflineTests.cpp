#include "ProceduralWind/WindOffline.h"

#include <DirectXPackedVector.h>
#include <cmath>
#include <filesystem>
#include <iostream>

int main()
{
    using namespace br::wind;
    const std::array<Float3, 4> positions{ Float3{ 0, 0, 0 }, Float3{ 8192, 0, 0 }, Float3{ 8192, 8192, 0 }, Float3{ 0, 8192, 0 } };
    const std::array<std::uint32_t, 6> indices{ 0, 1, 2, 0, 2, 3 };
    WindBakeSettings settings; settings.cellSize = 2048.0f; settings.directionCount = 16u;
    const auto bake = BakeLevel0WindField({ positions, indices }, 42u, settings);
    if (!bake.error.empty() || bake.directionRgba16f.size() != 16u) { std::cerr << bake.error; return 1; }
    const auto bracket = ComputeDirectionBracket(-0.01f, 16u);
    if (bracket.lower != 15u || bracket.upper != 0u) return 2;
    auto changedSettings = settings;
    changedSettings.cellSize *= 2.0f;
    if (ComputeWindBakeConfigHash(settings) == ComputeWindBakeConfigHash(changedSettings)) return 7;
    changedSettings = settings;
    changedSettings.directionCount = 8u;
    if (ComputeWindBakeConfigHash(settings) == ComputeWindBakeConfigHash(changedSettings)) return 8;
    const auto path = std::filesystem::temp_directory_path() / "procedural_wind_test.sarpwind";
    std::string error;
    if (!SaveWindCache(path, bake, &error)) { std::cerr << error; return 3; }
    WindCacheMetadata metadata;
    if (!LoadWindCacheMetadata(path, metadata, &error) || metadata.sourceHash != 42u) return 4;
    std::vector<std::uint16_t> loaded;
    if (!LoadWindDirection(path, metadata, 0u, loaded, &error) || loaded != bake.directionRgba16f[0]) return 5;
    for (std::size_t i = 0; i < loaded.size(); i += 4u) {
        const float x = DirectX::PackedVector::XMConvertHalfToFloat(loaded[i]);
        const float y = DirectX::PackedVector::XMConvertHalfToFloat(loaded[i + 1u]);
        if (!std::isfinite(x) || !std::isfinite(y)) return 6;
    }

    const std::array<Float3, 9> ridgePositions{
        Float3{ 0, 0, 0 }, Float3{ 4096, 0, 1024 }, Float3{ 8192, 0, 0 },
        Float3{ 0, 4096, 0 }, Float3{ 4096, 4096, 1024 }, Float3{ 8192, 4096, 0 },
        Float3{ 0, 8192, 0 }, Float3{ 4096, 8192, 1024 }, Float3{ 8192, 8192, 0 }
    };
    const std::array<std::uint32_t, 24> ridgeIndices{
        0, 1, 4, 0, 4, 3, 1, 2, 5, 1, 5, 4,
        3, 4, 7, 3, 7, 6, 4, 5, 8, 4, 8, 7
    };
    auto ridgeSettings = settings;
    ridgeSettings.cellSize = 1024.0f;
    const auto ridge = BakeLevel0WindField({ ridgePositions, ridgeIndices }, 43u, ridgeSettings);
    if (!ridge.error.empty()) return 9;
    bool sawUplift = false;
    for (std::size_t i = 2; i < ridge.directionRgba16f[0].size(); i += 4u) {
        const float z = DirectX::PackedVector::XMConvertHalfToFloat(ridge.directionRgba16f[0][i]);
        if (!std::isfinite(z) || std::abs(z) > ridgeSettings.maximumUpliftRatio + 0.001f) return 10;
        sawUplift = sawUplift || std::abs(z) > 0.001f;
    }
    if (!sawUplift) return 11;
    std::filesystem::remove(path);
    return 0;
}
