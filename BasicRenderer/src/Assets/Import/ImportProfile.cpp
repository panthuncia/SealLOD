#include <BasicRenderer/Assets/Import/ImportProfile.h>

#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace br::import
{
	namespace
	{
		using json = nlohmann::json;

		std::uint64_t Fnv1a(std::string_view text)
		{
			std::uint64_t hash = 1469598103934665603ull;
			for (const char c : text) {
				hash ^= static_cast<std::uint8_t>(c);
				hash *= 1099511628211ull;
			}
			return hash;
		}

		// Compiled once per pattern: SkipsMesh runs for every mesh of a scene.
		const std::regex& CompiledPattern(const std::string& pattern)
		{
			static std::mutex mutex;
			static std::unordered_map<std::string, std::unique_ptr<std::regex>> compiled;
			std::lock_guard lock(mutex);
			auto& entry = compiled[pattern];
			if (!entry) entry = std::make_unique<std::regex>(pattern, std::regex::ECMAScript | std::regex::optimize);
			return *entry;
		}

		std::uint32_t ChannelIndex(const json& value, const char* what)
		{
			if (!value.is_number_unsigned() || value.get<std::uint32_t>() > 3u) {
				throw std::runtime_error(std::string("import profile: '") + what + "' must be a channel index 0-3");
			}
			return value.get<std::uint32_t>();
		}

		bool EndsWith(std::string_view text, std::string_view suffix)
		{
			return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
		}
	}

	std::uint64_t ImportProfile::Hash() const
	{
		return Fnv1a(ImportProfileToJson(*this));
	}

	bool ImportProfile::SkipsMesh(std::string_view meshName) const
	{
		if (skipMeshes.empty()) return false;
		return std::regex_match(meshName.begin(), meshName.end(), CompiledPattern(skipMeshes));
	}

	const ImportProfileChannelRule* ImportProfile::FindMetallicRoughnessRule(std::uint32_t textureChannels, std::string_view imageName) const
	{
		for (const auto& rule : materialChannels) {
			if (rule.texture != "metallicRoughness") continue;
			if (rule.channels && *rule.channels != textureChannels) continue;
			if (!rule.nameSuffix.empty() && !EndsWith(imageName, rule.nameSuffix)) continue;
			return &rule;
		}
		return nullptr;
	}

	ImportProfile ParseImportProfile(std::string_view jsonText)
	{
		json document;
		try {
			document = json::parse(jsonText);
		}
		catch (const std::exception& error) {
			throw std::runtime_error(std::string("import profile: not JSON: ") + error.what());
		}
		if (!document.is_object()) throw std::runtime_error("import profile: must be a JSON object");

		ImportProfile profile;
		profile.name = document.value("name", std::string());
		profile.skipMeshes = document.value("skipMeshes", std::string());
		if (!profile.skipMeshes.empty()) {
			try {
				(void)CompiledPattern(profile.skipMeshes);
			}
			catch (const std::regex_error& error) {
				throw std::runtime_error("import profile: skipMeshes is not a valid regex: " + std::string(error.what()));
			}
		}
		if (document.contains("materialChannels")) {
			const auto& rules = document["materialChannels"];
			if (!rules.is_array()) throw std::runtime_error("import profile: materialChannels must be an array");
			for (const auto& node : rules) {
				ImportProfileChannelRule rule;
				rule.texture = node.value("texture", std::string());
				if (rule.texture != "metallicRoughness") {
					throw std::runtime_error("import profile: materialChannels texture '" + rule.texture + "' is not supported (metallicRoughness is)");
				}
				if (node.contains("channels")) rule.channels = node["channels"].get<std::uint32_t>();
				rule.nameSuffix = node.value("nameSuffix", std::string());
				if (node.contains("metallic")) rule.metallic = ChannelIndex(node["metallic"], "metallic");
				if (node.contains("roughness")) rule.roughness = ChannelIndex(node["roughness"], "roughness");
				profile.materialChannels.push_back(std::move(rule));
			}
		}
		return profile;
	}

	ImportProfile LoadImportProfile(const std::string& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream) throw std::runtime_error("import profile: cannot open '" + path + "'");
		const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		return ParseImportProfile(text);
	}

	std::string ImportProfileToJson(const ImportProfile& profile)
	{
		json document = json::object();
		document["name"] = profile.name;
		document["skipMeshes"] = profile.skipMeshes;
		json rules = json::array();
		for (const auto& rule : profile.materialChannels) {
			json node = json::object();
			node["texture"] = rule.texture;
			if (rule.channels) node["channels"] = *rule.channels;
			if (!rule.nameSuffix.empty()) node["nameSuffix"] = rule.nameSuffix;
			if (rule.metallic) node["metallic"] = *rule.metallic;
			if (rule.roughness) node["roughness"] = *rule.roughness;
			rules.push_back(std::move(node));
		}
		document["materialChannels"] = std::move(rules);
		return document.dump();
	}
}
