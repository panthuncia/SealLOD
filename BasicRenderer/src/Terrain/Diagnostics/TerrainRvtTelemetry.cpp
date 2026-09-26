#include <BasicRenderer/Diagnostics/TerrainRvtTelemetry.h>

#include <cstdlib>
#include <memory>
#include <string_view>

#include "Runtime/Settings/SettingsManager.h"

bool IsTerrainRvtTelemetryEnabledByEnvironment()
{
	char* value = nullptr;
	std::size_t len = 0;
	if (_dupenv_s(&value, &len, "SARP_TERRAIN_RVT_TELEMETRY") != 0 || value == nullptr) {
		return false;
	}

	const std::unique_ptr<char, decltype(&std::free)> valueStorage{ value, &std::free };
	const std::string_view setting{ valueStorage.get() };
	return !(setting == "0" || setting == "false" || setting == "FALSE" || setting == "off" || setting == "OFF");
}

bool IsTerrainRvtTelemetryDebugEnabled()
{
	if (IsTerrainRvtTelemetryEnabledByEnvironment()) {
		return true;
	}

	try {
		return SettingsManager::GetInstance().getSettingGetter<bool>(TerrainRvtTelemetryDebugSettingName)();
	}
	catch (...) {
		return false;
	}
}
