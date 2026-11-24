#pragma once

// Converts a setting's stored type to and from br::extensions::SettingValue (SettingAccess.h), so settings can be read and
// written by name without knowing their C++ type. Used by Setting<T>.

#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

#include <BasicRenderer/Extensions/SettingAccess.h>

namespace br::settings_detail {

	template <typename T> struct IsFunction : std::false_type {};
	template <typename S> struct IsFunction<std::function<S>> : std::true_type {};

	template <typename T> struct IsVector : std::false_type { using Element = void; };
	template <typename T, typename A> struct IsVector<std::vector<T, A>> : std::true_type { using Element = T; };

	// A small struct of numeric x/y[/z[/w]] members, such as DirectX::XMUINT2 or XMFLOAT2.
	template <typename T>
	concept PointLike = std::is_class_v<T> && requires(const T& t) {
		requires std::is_arithmetic_v<std::remove_cvref_t<decltype(t.x)>>;
		requires std::is_arithmetic_v<std::remove_cvref_t<decltype(t.y)>>;
	};

	inline std::string NumberTypeName(const std::type_info& type)
	{
		if (type == typeid(bool)) return "bool";
		if (type == typeid(float)) return "float";
		if (type == typeid(double)) return "double";
		if (type == typeid(std::int8_t)) return "int8";
		if (type == typeid(std::int16_t)) return "int16";
		if (type == typeid(std::int32_t)) return "int32";
		if (type == typeid(std::int64_t)) return "int64";
		if (type == typeid(std::uint8_t)) return "uint8";
		if (type == typeid(std::uint16_t)) return "uint16";
		if (type == typeid(std::uint32_t)) return "uint32";
		if (type == typeid(std::uint64_t)) return "uint64";
		return type.name();
	}

	template <typename T>
	std::string TypeName()
	{
		if constexpr (std::is_arithmetic_v<T>) return NumberTypeName(typeid(T));
		else if constexpr (std::is_enum_v<T>) {
			std::string name = typeid(T).name();  // "enum UpscalingMode" (MSVC ABI)
			return name.rfind("enum ", 0) == 0 ? name : "enum " + name;
		}
		else if constexpr (std::is_same_v<T, std::string>) return "string";
		else if constexpr (IsVector<T>::value) {
			using E = typename IsVector<T>::Element;
			if constexpr (std::is_same_v<E, std::string>) return "vector<string>";
			else if constexpr (std::is_arithmetic_v<E>) return "vector<" + NumberTypeName(typeid(E)) + ">";
			else return typeid(T).name();
		}
		else if constexpr (PointLike<T>) return std::string("point ") + typeid(T).name();
		else if constexpr (IsFunction<T>::value) return "callback";
		else return typeid(T).name();
	}

	// A number from any numeric alternative, converted to N; throws when it does not fit.
	template <typename N>
	N NumberFrom(const extensions::SettingValue& value)
	{
		if constexpr (std::is_same_v<N, bool>) {
			if (const bool* b = std::get_if<bool>(&value)) return *b;
			if (const auto* i = std::get_if<std::int64_t>(&value); i && (*i == 0 || *i == 1)) return *i != 0;
			if (const auto* u = std::get_if<std::uint64_t>(&value); u && *u <= 1) return *u != 0;
			throw std::runtime_error("expected a bool");
		} else if constexpr (std::is_floating_point_v<N>) {
			if (const double* d = std::get_if<double>(&value)) return static_cast<N>(*d);
			if (const auto* i = std::get_if<std::int64_t>(&value)) return static_cast<N>(*i);
			if (const auto* u = std::get_if<std::uint64_t>(&value)) return static_cast<N>(*u);
			throw std::runtime_error("expected a number");
		} else {
			static_assert(std::is_integral_v<N>);
			auto fits = [](auto v) {
				using V = decltype(v);
				if constexpr (std::is_signed_v<V>) {
					if (v < 0) return std::is_signed_v<N> && static_cast<std::int64_t>(v) >= static_cast<std::int64_t>(std::numeric_limits<N>::min());
					return static_cast<std::uint64_t>(v) <= static_cast<std::uint64_t>(std::numeric_limits<N>::max());
				} else {
					return v <= static_cast<std::uint64_t>(std::numeric_limits<N>::max());
				}
			};
			if (const auto* i = std::get_if<std::int64_t>(&value)) {
				if (!fits(*i)) throw std::runtime_error("integer out of range");
				return static_cast<N>(*i);
			}
			if (const auto* u = std::get_if<std::uint64_t>(&value)) {
				if (!fits(*u)) throw std::runtime_error("integer out of range");
				return static_cast<N>(*u);
			}
			if (const double* d = std::get_if<double>(&value)) {
				if (std::trunc(*d) != *d || std::abs(*d) > 9.0e15) throw std::runtime_error("expected an integer");
				return NumberFrom<N>(*d < 0 ? extensions::SettingValue{ static_cast<std::int64_t>(*d) }
										   : extensions::SettingValue{ static_cast<std::uint64_t>(*d) });
			}
			if (const bool* b = std::get_if<bool>(&value)) return static_cast<N>(*b ? 1 : 0);
			throw std::runtime_error("expected an integer");
		}
	}

	template <typename N>
	extensions::SettingValue NumberTo(N value)
	{
		if constexpr (std::is_same_v<N, bool>) return value;
		else if constexpr (std::is_floating_point_v<N>) return static_cast<double>(value);
		else if constexpr (std::is_signed_v<N>) return static_cast<std::int64_t>(value);
		else return static_cast<std::uint64_t>(value);
	}

	template <typename T>
	extensions::SettingValue ToValue(const T& value)
	{
		if constexpr (std::is_arithmetic_v<T>) return NumberTo(value);
		else if constexpr (std::is_enum_v<T>) return NumberTo(static_cast<std::underlying_type_t<T>>(value));
		else if constexpr (std::is_same_v<T, std::string>) return value;
		else if constexpr (IsVector<T>::value) {
			using E = typename IsVector<T>::Element;
			if constexpr (std::is_same_v<E, std::string>) return std::vector<std::string>(value.begin(), value.end());
			else if constexpr (std::is_arithmetic_v<E>) {
				std::vector<double> list;
				for (const auto& element : value) list.push_back(static_cast<double>(element));
				return list;
			}
			else return std::monostate{};
		}
		else if constexpr (PointLike<T>) {
			std::vector<double> list{ static_cast<double>(value.x), static_cast<double>(value.y) };
			if constexpr (requires { value.z; }) list.push_back(static_cast<double>(value.z));
			if constexpr (requires { value.w; }) list.push_back(static_cast<double>(value.w));
			return list;
		}
		else return std::monostate{};
	}

	template <typename T>
	T FromValue(const extensions::SettingValue& value, const T& current)
	{
		if constexpr (std::is_arithmetic_v<T>) return NumberFrom<T>(value);
		else if constexpr (std::is_enum_v<T>) return static_cast<T>(NumberFrom<std::underlying_type_t<T>>(value));
		else if constexpr (std::is_same_v<T, std::string>) {
			if (const auto* s = std::get_if<std::string>(&value)) return *s;
			throw std::runtime_error("expected a string");
		}
		else if constexpr (IsVector<T>::value && std::is_same_v<typename IsVector<T>::Element, std::string>) {
			if (const auto* list = std::get_if<std::vector<std::string>>(&value)) return T(list->begin(), list->end());
			if (const auto* d = std::get_if<std::vector<double>>(&value); d && d->empty()) return T{};
			throw std::runtime_error("expected a list of strings");
		}
		else if constexpr (IsVector<T>::value && std::is_arithmetic_v<typename IsVector<T>::Element>) {
			const auto* list = std::get_if<std::vector<double>>(&value);
			if (!list) throw std::runtime_error("expected a list of numbers");
			T result;
			for (double element : *list) result.push_back(NumberFrom<typename IsVector<T>::Element>(element));
			return result;
		}
		else if constexpr (PointLike<T>) {
			const auto* list = std::get_if<std::vector<double>>(&value);
			const std::size_t size = std::get<std::vector<double>>(ToValue(current)).size();
			if (!list || list->size() != size) throw std::runtime_error("expected a list of " + std::to_string(size) + " numbers");
			T result = current;
			result.x = NumberFrom<std::remove_cvref_t<decltype(result.x)>>((*list)[0]);
			result.y = NumberFrom<std::remove_cvref_t<decltype(result.y)>>((*list)[1]);
			if constexpr (requires { result.z; }) result.z = NumberFrom<std::remove_cvref_t<decltype(result.z)>>((*list)[2]);
			if constexpr (requires { result.w; }) result.w = NumberFrom<std::remove_cvref_t<decltype(result.w)>>((*list)[3]);
			return result;
		}
		else {
			(void)value;
			throw std::runtime_error("a setting of type " + TypeName<T>() + " cannot be written by name");
		}
	}

	// Whether two values are known to be equal. Types with no way to compare (callbacks) are never equal, so writing one always
	// notifies.
	template <typename T>
	bool KnownEqual(const T& a, const T& b)
	{
		if constexpr (PointLike<T>) return ToValue(a) == ToValue(b);
		else if constexpr (std::equality_comparable<T>) return a == b;
		else return false;
	}

} // namespace br::settings_detail
