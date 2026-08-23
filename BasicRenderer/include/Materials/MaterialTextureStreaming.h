#pragma once

#include <cstdint>

#include "Managers/Singletons/SettingsManager.h"

inline constexpr const char* MaterialTextureStreamingSettingName = "enableMaterialTextureStreaming";
inline constexpr const char* AlphaTestedMaterialTextureMaxResidentTopMipSettingName =
    "alphaTestedMaterialTextureMaxResidentTopMip";
inline constexpr uint32_t AlphaTestedMaterialTextureMaxResidentTopMipDefault = 4u;

inline bool IsMaterialTextureStreamingEnabledSetting() {
    try {
        return SettingsManager::GetInstance().getSettingGetter<bool>(MaterialTextureStreamingSettingName)();
    }
    catch (...) {
        return true;
    }
}

inline uint32_t GetAlphaTestedMaterialTextureMaxResidentTopMipSetting() {
    try {
        return SettingsManager::GetInstance().getSettingGetter<uint32_t>(
            AlphaTestedMaterialTextureMaxResidentTopMipSettingName)();
    }
    catch (...) {
        return AlphaTestedMaterialTextureMaxResidentTopMipDefault;
    }
}
