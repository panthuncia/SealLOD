#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

// A setting's stored type, for callers that address settings by name at run time (scripts, control commands). Other is a
// type with no accessor here (an enum, a callback); Missing is no setting by that name.
enum class SettingValueType : std::uint8_t { Missing, Bool, Float, UInt32, Int32, UInt8, Other };
SettingValueType GetSettingValueType(const std::string& name);

// Any setting by name, as a plain value: for control commands and scripts that do not know the C++ type.
// - bool, signed and unsigned integers, floats and strings map to the alternative of that kind;
// - an enum maps to its underlying integer;
// - a vector of numbers or strings, or a small struct of x/y/z/w numbers (DirectX::XMUINT2), maps to a list.
// Anything else (callbacks) is monostate: readable as "not representable" and never writable.
using SettingValue = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, std::vector<double>,
	std::vector<std::string>>;

// Every registered setting's name, sorted.
std::vector<std::string> ListSettingNames();
// "bool", "float", "uint32", "enum UpscalingMode", "vector<float>", "callback", ...; empty when there is no such setting.
std::string DescribeSettingType(const std::string& name);
// Throws std::runtime_error when there is no setting by that name.
SettingValue ReadSettingValue(const std::string& name);
// Converts the value to the setting's type (integers range-checked, an integral double accepted for an integer) and writes
// it, which runs the setting's observers. Returns false, and writes nothing, when the setting already holds that value.
// Throws std::runtime_error when there is no such setting, or the value does not fit its type.
bool WriteSettingValue(const std::string& name, const SettingValue& value);


template <typename T>
std::function<T()> MakeSettingGetter(const std::string& name);

template <typename T>
std::function<void(T)> MakeSettingSetter(const std::string& name);

template <> std::function<bool()> MakeSettingGetter<bool>(const std::string& name);
template <> std::function<float()> MakeSettingGetter<float>(const std::string& name);
template <> std::function<std::uint32_t()> MakeSettingGetter<std::uint32_t>(const std::string& name);
template <> std::function<std::int32_t()> MakeSettingGetter<std::int32_t>(const std::string& name);
template <> std::function<std::uint8_t()> MakeSettingGetter<std::uint8_t>(const std::string& name);
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
