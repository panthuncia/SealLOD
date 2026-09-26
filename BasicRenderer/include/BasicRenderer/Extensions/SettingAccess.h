#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

class SettingsManager;
enum class UpscaleQualityMode;
enum class CLodVSMRasterMode : std::uint8_t;
enum class CLodSoftwareRasterMode : std::uint8_t;
enum class CLodCullingBackend : std::uint8_t;
enum class CLodTransparencyMode : std::uint8_t;
namespace org { enum class AutoAliasMode : std::uint8_t; }

namespace br::extensions {

float ReadFloatSetting(const std::string& name);
bool ReadBoolSetting(const std::string& name);
std::uint32_t ReadUnsignedSetting(const std::string& name);


template <typename T>
std::function<T()> MakeSettingGetter(const std::string& name);

template <typename T>
std::function<void(T)> MakeSettingSetter(const std::string& name);

template <> std::function<bool()> MakeSettingGetter<bool>(const std::string& name);
template <> std::function<float()> MakeSettingGetter<float>(const std::string& name);
template <> std::function<std::uint32_t()> MakeSettingGetter<std::uint32_t>(const std::string& name);
template <> std::function<std::int32_t()> MakeSettingGetter<std::int32_t>(const std::string& name);
template <> std::function<CLodVSMRasterMode()> MakeSettingGetter<CLodVSMRasterMode>(const std::string& name);
template <> std::function<CLodSoftwareRasterMode()> MakeSettingGetter<CLodSoftwareRasterMode>(const std::string& name);
template <> std::function<CLodCullingBackend()> MakeSettingGetter<CLodCullingBackend>(const std::string& name);
template <> std::function<CLodTransparencyMode()> MakeSettingGetter<CLodTransparencyMode>(const std::string& name);

template <> std::function<void(bool)> MakeSettingSetter<bool>(const std::string& name);
template <> std::function<void(float)> MakeSettingSetter<float>(const std::string& name);
template <> std::function<void(std::uint32_t)> MakeSettingSetter<std::uint32_t>(const std::string& name);
template <> std::function<void(std::int32_t)> MakeSettingSetter<std::int32_t>(const std::string& name);
template <> std::function<void(std::uint8_t)> MakeSettingSetter<std::uint8_t>(const std::string& name);
template <> std::function<void(org::AutoAliasMode)> MakeSettingSetter<org::AutoAliasMode>(const std::string& name);
template <> std::function<void(UpscaleQualityMode)> MakeSettingSetter<UpscaleQualityMode>(const std::string& name);
template <> std::function<void(CLodVSMRasterMode)> MakeSettingSetter<CLodVSMRasterMode>(const std::string& name);
template <> std::function<void(CLodCullingBackend)> MakeSettingSetter<CLodCullingBackend>(const std::string& name);
template <> std::function<void(CLodSoftwareRasterMode)> MakeSettingSetter<CLodSoftwareRasterMode>(const std::string& name);
template <> std::function<void(CLodTransparencyMode)> MakeSettingSetter<CLodTransparencyMode>(const std::string& name);


class SettingSubscription {
public:
	SettingSubscription() = default;
	SettingSubscription(SettingSubscription&& other) noexcept
		: _unsubscribe(std::move(other._unsubscribe))
	{
		other._unsubscribe = nullptr;
	}
	SettingSubscription& operator=(SettingSubscription&& other) noexcept
	{
		if (this != &other) {
			cancel();
			_unsubscribe = std::move(other._unsubscribe);
			other._unsubscribe = nullptr;
		}
		return *this;
	}
	~SettingSubscription() { cancel(); }

	void cancel()
	{
		if (_unsubscribe) {
			_unsubscribe();
			_unsubscribe = nullptr;
		}
	}

	SettingSubscription(const SettingSubscription&) = delete;
	SettingSubscription& operator=(const SettingSubscription&) = delete;

private:
	friend class ::SettingsManager;
	explicit SettingSubscription(std::function<void()> unsubscribe)
		: _unsubscribe(std::move(unsubscribe))
	{
	}
	std::function<void()> _unsubscribe;
};

} // namespace br::extensions
