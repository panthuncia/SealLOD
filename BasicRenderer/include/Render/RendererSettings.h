#pragma once

#include <algorithm>
#include <cstdint>

#include <DirectXMath.h>

inline constexpr const char* WindowResolutionPresetSettingName = "windowResolutionPreset";

enum class WindowResolutionPreset : uint8_t {
    P720,
    P1080,
    P1440,
    P2160,
};

inline constexpr const char* WindowResolutionPresetNames[] = {
    "1280 x 720",
    "1920 x 1080",
    "2560 x 1440",
    "3840 x 2160",
};
inline constexpr int WindowResolutionPresetCount =
    static_cast<int>(sizeof(WindowResolutionPresetNames) / sizeof(WindowResolutionPresetNames[0]));

inline DirectX::XMUINT2 ResolveWindowResolutionPreset(WindowResolutionPreset preset)
{
    switch (preset) {
    case WindowResolutionPreset::P720:
        return { 1280u, 720u };
    case WindowResolutionPreset::P1080:
        return { 1920u, 1080u };
    case WindowResolutionPreset::P1440:
        return { 2560u, 1440u };
    case WindowResolutionPreset::P2160:
        return { 3840u, 2160u };
    default:
        return { 1920u, 1080u };
    }
}

inline WindowResolutionPreset FindClosestWindowResolutionPreset(uint32_t width, uint32_t height)
{
    WindowResolutionPreset bestPreset = WindowResolutionPreset::P1080;
    uint64_t bestDistance = UINT64_MAX;
    for (uint32_t i = 0; i < static_cast<uint32_t>(WindowResolutionPresetCount); ++i) {
        const auto preset = static_cast<WindowResolutionPreset>(i);
        const auto resolution = ResolveWindowResolutionPreset(preset);
        const auto dx = static_cast<int64_t>(resolution.x) - static_cast<int64_t>(width);
        const auto dy = static_cast<int64_t>(resolution.y) - static_cast<int64_t>(height);
        const uint64_t distance = static_cast<uint64_t>(dx * dx + dy * dy);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestPreset = preset;
        }
    }
    return bestPreset;
}
