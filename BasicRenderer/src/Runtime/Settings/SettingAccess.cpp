#include <BasicRenderer/Extensions/SettingAccess.h>

#include <BasicRenderer/Pipeline/CLodOptions.h>
#include <BasicRenderer/Pipeline/UpscalingTypes.h>
#include <OpenRenderGraph/OpenRenderGraph.h>

#include "Runtime/Settings/SettingsManager.h"

namespace br::extensions {

float ReadFloatSetting(const std::string& name)
{
    return SettingsManager::GetInstance().getSettingGetter<float>(name)();
}

bool ReadBoolSetting(const std::string& name)
{
    return SettingsManager::GetInstance().getSettingGetter<bool>(name)();
}

std::uint32_t ReadUnsignedSetting(const std::string& name)
{
    return SettingsManager::GetInstance().getSettingGetter<std::uint32_t>(name)();
}

template <> std::function<bool()> MakeSettingGetter<bool>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<bool>(name);
}

template <> std::function<float()> MakeSettingGetter<float>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<float>(name);
}

template <> std::function<std::uint32_t()> MakeSettingGetter<std::uint32_t>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<std::uint32_t>(name);
}

template <> std::function<std::int32_t()> MakeSettingGetter<std::int32_t>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<std::int32_t>(name);
}

template <> std::function<CLodVSMRasterMode()> MakeSettingGetter<CLodVSMRasterMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<CLodVSMRasterMode>(name);
}

template <> std::function<CLodSoftwareRasterMode()> MakeSettingGetter<CLodSoftwareRasterMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<CLodSoftwareRasterMode>(name);
}

template <> std::function<CLodCullingBackend()> MakeSettingGetter<CLodCullingBackend>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<CLodCullingBackend>(name);
}

template <> std::function<CLodTransparencyMode()> MakeSettingGetter<CLodTransparencyMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingGetter<CLodTransparencyMode>(name);
}

template <> std::function<void(bool)> MakeSettingSetter<bool>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<bool>(name);
}

template <> std::function<void(float)> MakeSettingSetter<float>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<float>(name);
}

template <> std::function<void(std::uint32_t)> MakeSettingSetter<std::uint32_t>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<std::uint32_t>(name);
}

template <> std::function<void(std::int32_t)> MakeSettingSetter<std::int32_t>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<std::int32_t>(name);
}

template <> std::function<void(std::uint8_t)> MakeSettingSetter<std::uint8_t>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<std::uint8_t>(name);
}

template <> std::function<void(org::AutoAliasMode)> MakeSettingSetter<org::AutoAliasMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<org::AutoAliasMode>(name);
}

template <> std::function<void(UpscaleQualityMode)> MakeSettingSetter<UpscaleQualityMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<UpscaleQualityMode>(name);
}

template <> std::function<void(CLodVSMRasterMode)> MakeSettingSetter<CLodVSMRasterMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<CLodVSMRasterMode>(name);
}

template <> std::function<void(CLodCullingBackend)> MakeSettingSetter<CLodCullingBackend>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<CLodCullingBackend>(name);
}

template <> std::function<void(CLodSoftwareRasterMode)> MakeSettingSetter<CLodSoftwareRasterMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<CLodSoftwareRasterMode>(name);
}

template <> std::function<void(CLodTransparencyMode)> MakeSettingSetter<CLodTransparencyMode>(const std::string& name)
{
	return SettingsManager::GetInstance().getSettingSetter<CLodTransparencyMode>(name);
}

} // namespace br::extensions
