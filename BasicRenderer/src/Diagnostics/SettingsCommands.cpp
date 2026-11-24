#include <BasicRenderer/Diagnostics/SettingsCommands.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <stdexcept>
#include <variant>

namespace br::diagnostics {

using br::extensions::SettingValue;

nlohmann::json SettingValueToJson(const SettingValue& value)
{
	return std::visit([](const auto& v) -> nlohmann::json {
		using V = std::decay_t<decltype(v)>;
		if constexpr (std::is_same_v<V, std::monostate>) return nullptr;
		else return v;
	}, value);
}

SettingValue SettingValueFromJson(const nlohmann::json& value)
{
	switch (value.type()) {
	case nlohmann::json::value_t::boolean: return value.get<bool>();
	case nlohmann::json::value_t::number_integer: return value.get<std::int64_t>();
	case nlohmann::json::value_t::number_unsigned: return value.get<std::uint64_t>();
	case nlohmann::json::value_t::number_float: return value.get<double>();
	case nlohmann::json::value_t::string: return value.get<std::string>();
	case nlohmann::json::value_t::array: {
		if (value.empty()) return std::vector<double>{};
		if (std::all_of(value.begin(), value.end(), [](const nlohmann::json& e) { return e.is_number(); })) {
			std::vector<double> list;
			for (const auto& element : value) list.push_back(element.get<double>());
			return list;
		}
		if (std::all_of(value.begin(), value.end(), [](const nlohmann::json& e) { return e.is_string(); })) {
			return value.get<std::vector<std::string>>();
		}
		throw std::runtime_error("a list value must be all numbers or all strings");
	}
	default: throw std::runtime_error("a setting value is a bool, number, string or list");
	}
}

namespace {

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return text;
	}

	// Every representable setting's value, to find what a write changed (constraints and observers write others).
	std::map<std::string, SettingValue> SnapshotValues()
	{
		std::map<std::string, SettingValue> values;
		for (const auto& name : br::extensions::ListSettingNames()) values.emplace(name, br::extensions::ReadSettingValue(name));
		return values;
	}

} // namespace

void RegisterSettingsCommands(ControlCommandRegistry& commands)
{
	commands.Register("settings.list", [](const ControlRequest& request) {
		const std::string match = Lower(request.args.value("match", std::string()));
		nlohmann::json list = nlohmann::json::array();
		for (const auto& name : br::extensions::ListSettingNames()) {
			if (!match.empty() && Lower(name).find(match) == std::string::npos) continue;
			list.push_back({ { "name", name },
				{ "type", br::extensions::DescribeSettingType(name) },
				{ "value", SettingValueToJson(br::extensions::ReadSettingValue(name)) } });
		}
		return nlohmann::json{ { "settings", std::move(list) } };
	});

	commands.Register("settings.get", [](const ControlRequest& request) {
		nlohmann::json response = nlohmann::json::object();
		if (const auto name = request.args.find("name"); name != request.args.end()) {
			response["value"] = SettingValueToJson(br::extensions::ReadSettingValue(name->get<std::string>()));
			response["type"] = br::extensions::DescribeSettingType(name->get<std::string>());
		}
		if (const auto names = request.args.find("names"); names != request.args.end()) {
			nlohmann::json values = nlohmann::json::object();
			for (const auto& entry : *names) {
				const auto key = entry.get<std::string>();
				values[key] = SettingValueToJson(br::extensions::ReadSettingValue(key));
			}
			response["values"] = std::move(values);
		}
		if (response.empty()) throw std::runtime_error("settings.get takes \"name\" or \"names\"");
		return response;
	});

	commands.Register("settings.set", [](const ControlRequest& request) {
		std::vector<std::pair<std::string, SettingValue>> writes;
		if (const auto name = request.args.find("name"); name != request.args.end()) {
			if (!request.args.contains("value")) throw std::runtime_error("settings.set with \"name\" needs \"value\"");
			writes.emplace_back(name->get<std::string>(), SettingValueFromJson(request.args.at("value")));
		}
		if (const auto values = request.args.find("values"); values != request.args.end()) {
			if (!values->is_object()) throw std::runtime_error("\"values\" is an object of name: value");
			for (const auto& [key, value] : values->items()) writes.emplace_back(key, SettingValueFromJson(value));
		}
		if (writes.empty()) throw std::runtime_error("settings.set takes \"name\" and \"value\", or \"values\"");

		const auto before = SnapshotValues();
		std::string failure;
		for (const auto& [key, value] : writes) {
			try {
				br::extensions::WriteSettingValue(key, value);
			} catch (const std::exception& error) {
				failure = error.what();
				break;
			}
		}
		nlohmann::json response = nlohmann::json::object();
		nlohmann::json changed = nlohmann::json::object();
		for (const auto& [key, value] : SnapshotValues()) {
			const auto old = before.find(key);
			if (old == before.end() || old->second != value) changed[key] = SettingValueToJson(value);
		}
		nlohmann::json readBack = nlohmann::json::object();
		for (const auto& [key, value] : writes) readBack[key] = SettingValueToJson(br::extensions::ReadSettingValue(key));
		if (!failure.empty()) {
			// Earlier writes in the batch stay applied; say which.
			throw std::runtime_error(failure + " (changed so far: " + changed.dump() + ")");
		}
		if (request.args.contains("name")) response["value"] = readBack[request.args.at("name").get<std::string>()];
		response["values"] = std::move(readBack);
		response["changed"] = std::move(changed);
		return response;
	});
}

} // namespace br::diagnostics
