#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <rhi.h>

#include "Runtime/Settings/SettingsManager.h"
#include "BasicRenderer/Extensions/VirtualGeometry/CLodCommon.h"
#include <BasicRenderer/Diagnostics/OutputTypes.h>
#include "BasicRenderer/Scene/SceneIngestionServices.h"

inline br::render::SceneIngestionConfiguration CaptureSceneIngestionConfiguration()
{
    auto& settings = SettingsManager::GetInstance();
    br::render::SceneIngestionConfiguration result{};
    result.renderResolution = settings.getSettingGetter<DirectX::XMUINT2>("renderResolution")();
    result.outputResolution = settings.getSettingGetter<DirectX::XMUINT2>("outputResolution")();
    result.shadowResolution = settings.getSettingGetter<uint16_t>("shadowResolution")();
    result.directionalCascadeCount = settings.getSettingGetter<uint8_t>("numDirectionalLightCascades")();
    result.maxShadowDistance = settings.getSettingGetter<float>("maxShadowDistance")();
    result.directionalShadowDistanceLowerBound =
        settings.getSettingGetter<float>("directionalShadowDistanceLowerBound")();
    result.directionalShadowSceneExtent =
        settings.getSettingGetter<float>("directionalShadowSceneExtent")();
    result.primaryCameraUsesOutputHeight =
        settings.getSettingGetter<CLodLodHeightMode>(CLodLodHeightModeSettingName)() ==
        CLodLodHeightMode::OutputHeight;
    return result;
}

inline uint32_t PrimaryCameraLodHeight(const br::render::SceneIngestionConfiguration& configuration)
{
    return configuration.primaryCameraUsesOutputHeight && configuration.outputResolution.y != 0u
        ? configuration.outputResolution.y : configuration.renderResolution.y;
}

inline bool OutputTypeRequiresRenderGraphRebuild(unsigned int outputType)
{
    return outputType == static_cast<unsigned int>(OutputType::SKELETONS);
}

void SyncOpenRenderGraphSettings(uint8_t numFramesInFlight);

inline void ProbeGraphicsCommandListCreation(rhi::Device device, std::string_view phase) {
    (void)device;
    (void)phase;
}

namespace br::runtime::renderer_settings {
inline bool ReadTruthyEnvironmentFlag(const char* name) {
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) return false;
    const bool result = std::strcmp(value, "1") == 0 || _stricmp(value, "true") == 0
        || _stricmp(value, "yes") == 0 || _stricmp(value, "on") == 0;
    std::free(value);
    return result;
}

inline bool IsStreamlineDisabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_STREAMLINE") != 0 || value == nullptr) return false;
    const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    std::free(value);
    return disabled;
}

inline bool IsDirectStorageDisabledByEnvironment() {
    char* value = nullptr;
    size_t len = 0;
    if (_dupenv_s(&value, &len, "BASICRENDERER_DISABLE_DIRECTSTORAGE") != 0 || value == nullptr) return false;
    const bool disabled = value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
    std::free(value);
    return disabled;
}

inline bool DefaultEnableReShapeForBuild() {
#if BASICRHI_ENABLE_RESHAPE
    return true;
#else
    return false;
#endif
}
}
