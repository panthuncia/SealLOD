#include "BasicRenderer/Assets/MaterialTextureStreaming.h"

#include "Runtime/Settings/SettingsManager.h"

bool IsMaterialTextureStreamingEnabledSetting()
{
	try {
		return SettingsManager::GetInstance().getSettingGetter<bool>(MaterialTextureStreamingSettingName)();
	}
	catch (...) {
		return true;
	}
}

uint32_t GetAlphaTestedMaterialTextureMaxResidentTopMipSetting()
{
	try {
		return SettingsManager::GetInstance().getSettingGetter<uint32_t>(
			AlphaTestedMaterialTextureMaxResidentTopMipSettingName)();
	}
	catch (...) {
		return AlphaTestedMaterialTextureMaxResidentTopMipDefault;
	}
}

uint32_t GetAlphaTestedMaterialTextureMinResidentDimensionSetting()
{
	try {
		return SettingsManager::GetInstance().getSettingGetter<uint32_t>(
			AlphaTestedMaterialTextureMinResidentDimensionSettingName)();
	}
	catch (...) {
		return AlphaTestedMaterialTextureMinResidentDimensionDefault;
	}
}
