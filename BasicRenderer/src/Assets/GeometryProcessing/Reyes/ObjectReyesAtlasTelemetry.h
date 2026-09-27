#pragma once

// Enables the Object Reyes atlas shader telemetry that reyesPatchRaster.hlsl
// already contains but compiles out by default
// (CLOD_REYES_PATCH_RASTER_ATLAS_DEBUG_TELEMETRY defaults to 0). It reports, from
// inside the patch rasterizer, how many atlas-height materials reach it, their
// material-slot and height-descriptor ranges, whether displacement was enabled,
// how many carried the zero "no texture" descriptor, and whether the height UV
// set the material asks for actually exists in the CLod page header.
//
// Mirrors TerrainRvtTelemetry.h so that the setting and the shader define are
// driven from one place, and so the telemetry can be turned on from a benchmark
// script (which cannot reach the debug menu) through the environment.

#include <cstdlib>
#include <memory>
#include <string_view>

#include "Runtime/Settings/SettingsManager.h"

inline constexpr const char* ObjectReyesAtlasTelemetryDebugSetting = "objectReyesAtlasTelemetryDebug";

inline bool IsObjectReyesAtlasTelemetryEnabledByEnvironment()
{
    char* value = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&value, &len, "SARP_OBJECT_REYES_ATLAS_TELEMETRY") != 0 || value == nullptr) {
        return false;
    }

    const std::unique_ptr<char, decltype(&std::free)> valueStorage{ value, &std::free };
    const std::string_view setting{ valueStorage.get() };
    return !(setting == "0" || setting == "false" || setting == "FALSE" || setting == "off" || setting == "OFF");
}

inline bool IsObjectReyesAtlasTelemetryDebugEnabled()
{
    if (IsObjectReyesAtlasTelemetryEnabledByEnvironment()) {
        return true;
    }

    try {
        return SettingsManager::GetInstance().getSettingGetter<bool>(ObjectReyesAtlasTelemetryDebugSetting)();
    }
    catch (...) {
        return false;
    }
}
