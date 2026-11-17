#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "BasicRenderer/Extensions/RenderContext.h"
#include "Runtime/Settings/SettingsManager.h"

// Screen-space ambient occlusion settings. enableGTAO stays the on/off switch (it follows GtaoTechnique in the recipe, the
// screen-space AO slot), and ambientOcclusionTechnique picks what fills that slot when the graph is built.
namespace br::ambient_occlusion {

enum class AmbientOcclusionTechnique : uint32_t {
    XeGTAO = 0,
    CORTAO = 1,
};

inline constexpr const char* TechniqueSettingName = "ambientOcclusionTechnique";
inline constexpr uint32_t TechniqueDefault = static_cast<uint32_t>(AmbientOcclusionTechnique::CORTAO);

// CORTAO (docs/lighting.md "Ambient occlusion"). The sample count is a pipeline define, so changing it rebuilds the graph;
// the rest are read every frame.
inline constexpr const char* CortaoSampleCountSettingName = "cortaoSampleCount";
inline constexpr const char* CortaoRadiusSettingName = "cortaoRadius";
inline constexpr const char* CortaoScreenRadiusRatioSettingName = "cortaoScreenRadiusRatio";
inline constexpr const char* CortaoMipBiasSettingName = "cortaoMipBias";
inline constexpr const char* CortaoSlopeCompensationSettingName = "cortaoSlopeCompensation";
inline constexpr const char* CortaoBackfaceThresholdSettingName = "cortaoBackfaceThreshold";
inline constexpr const char* CortaoBackfaceBoostSettingName = "cortaoBackfaceBoost";
inline constexpr const char* CortaoFilterDepthSensitivitySettingName = "cortaoFilterDepthSensitivity";
inline constexpr const char* CortaoFilterNormalSensitivitySettingName = "cortaoFilterNormalSensitivity";

// CORTAO's quality presets, Minimal to Ultra.
inline constexpr std::array<uint32_t, 7> CortaoSampleCounts = { 4u, 6u, 8u, 12u, 16u, 24u, 32u };
inline constexpr uint32_t CortaoSampleCountDefault = 8u;
inline constexpr float CortaoRadiusDefault = 1.0f; // World units.
inline constexpr float CortaoScreenRadiusRatioDefault = 0.2f;
inline constexpr float CortaoMipBiasDefault = -4.0f;
inline constexpr float CortaoSlopeCompensationDefault = 4.0f;
inline constexpr float CortaoBackfaceThresholdDefault = 0.5f;
inline constexpr float CortaoBackfaceBoostDefault = 0.0f;
inline constexpr float CortaoFilterDepthSensitivityDefault = 512.0f;
inline constexpr float CortaoFilterNormalSensitivityDefault = 128.0f;

// The supported sample count closest to a requested one (the setting is free-form; the shader has one pipeline per preset).
inline uint32_t ClampCortaoSampleCount(uint32_t requested) {
    uint32_t best = CortaoSampleCounts.front();
    for (const uint32_t count : CortaoSampleCounts) {
        const uint32_t distance = count > requested ? count - requested : requested - count;
        const uint32_t bestDistance = best > requested ? best - requested : requested - best;
        if (distance < bestDistance) best = count;
    }
    return best;
}

inline AmbientOcclusionTechnique ReadAmbientOcclusionTechnique() {
    const uint32_t value = SettingsManager::GetInstance().getSettingGetter<uint32_t>(TechniqueSettingName)();
    return value == static_cast<uint32_t>(AmbientOcclusionTechnique::XeGTAO) ? AmbientOcclusionTechnique::XeGTAO
                                                                            : AmbientOcclusionTechnique::CORTAO;
}

// What lighting reads, from the settings as a graph is built: off without enableGTAO.
inline ScreenSpaceAOMode ReadScreenSpaceAOMode() {
    if (!SettingsManager::GetInstance().getSettingGetter<bool>("enableGTAO")()) return ScreenSpaceAOMode::Off;
    return ReadAmbientOcclusionTechnique() == AmbientOcclusionTechnique::XeGTAO ? ScreenSpaceAOMode::XeGTAO : ScreenSpaceAOMode::CORTAO;
}

}
