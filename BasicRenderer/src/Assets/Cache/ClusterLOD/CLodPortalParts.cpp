#include <BasicRenderer/Assets/Import/CLodPortalParts.h>

#include <format>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace CLodCache {

namespace {
	// Cache entries live at <tree>/<entry directory>/<file>.clodmeta. Part paths are stored relative to <tree>.
	std::filesystem::path TreeOf(const std::filesystem::path& metadataPath)
	{
		return metadataPath.parent_path().parent_path();
	}
}

std::filesystem::path PortalPartsPath(const std::filesystem::path& rootMetadataPath)
{
	std::filesystem::path path = rootMetadataPath;
	path.replace_extension(".clodparts");
	return path;
}

bool SavePortalParts(const CacheKey& rootKey, uint64_t rootBuildConfigHash, const PortalParts& parts)
{
	const std::filesystem::path rootMetadata = BuildCacheLookup(rootKey, rootBuildConfigHash).metadataPath;
	const std::filesystem::path tree = TreeOf(rootMetadata);
	nlohmann::json json;
	json["version"] = kPortalPartsVersion;
	json["root"] = { { "source", rootKey.sourceIdentifier }, { "prim", rootKey.primPath }, { "subset", rootKey.subsetName } };
	nlohmann::json list = nlohmann::json::array();
	for (const PortalPart& part : parts.parts) {
		const std::filesystem::path metadata = BuildCacheLookup(part.key, part.buildConfigHash).metadataPath;
		std::error_code ec;
		std::filesystem::path relative = std::filesystem::relative(metadata, tree, ec);
		if (ec || relative.empty()) {
			relative = metadata;
		}
		list.push_back({
			{ "source", part.key.sourceIdentifier },
			{ "prim", part.key.primPath },
			{ "subset", part.key.subsetName },
			{ "build_config_hash", std::format("{:016X}", part.buildConfigHash) },
			{ "label", part.label },
			{ "metadata", relative.generic_string() },
		});
	}
	json["parts"] = std::move(list);
	json["palette_parts"] = parts.paletteParts;

	const std::filesystem::path path = PortalPartsPath(rootMetadata);
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::filesystem::path temporary = path;
	temporary += ".tmp";
	{
		std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false;
		}
		out << json.dump(1, '\t') << '\n';
		if (!out) {
			return false;
		}
	}
	ec.clear();
	std::filesystem::rename(temporary, path, ec);
	if (ec) {
		// Replacing a file can fail through a virtual filesystem; remove it and retry.
		ec.clear();
		std::filesystem::remove(path, ec);
		ec.clear();
		std::filesystem::rename(temporary, path, ec);
	}
	if (ec) {
		std::error_code removeEc;
		std::filesystem::remove(temporary, removeEc);
		return false;
	}
	return true;
}

std::optional<PortalParts> LoadPortalParts(const std::filesystem::path& rootMetadataPath, std::string* outError)
{
	const auto fail = [&](std::string message) -> std::optional<PortalParts> {
		if (outError) {
			*outError = std::move(message);
		}
		return std::nullopt;
	};
	const std::filesystem::path path = PortalPartsPath(rootMetadataPath);
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		return fail(std::format("no portal parts file '{}'", path.string()));
	}
	try {
		const nlohmann::json json = nlohmann::json::parse(in);
		if (json.value("version", 0u) != kPortalPartsVersion) {
			return fail(std::format("'{}' has version {}, not {}", path.string(), json.value("version", 0u), kPortalPartsVersion));
		}
		const std::filesystem::path tree = TreeOf(rootMetadataPath);
		PortalParts out;
		for (const nlohmann::json& entry : json.at("parts")) {
			PortalPart part;
			part.key = { entry.at("source").get<std::string>(), entry.at("prim").get<std::string>(), entry.at("subset").get<std::string>() };
			part.buildConfigHash = std::stoull(entry.at("build_config_hash").get<std::string>(), nullptr, 16);
			part.label = entry.value("label", std::string());
			const std::filesystem::path metadata = std::filesystem::path(entry.at("metadata").get<std::string>());
			part.metadataPath = metadata.is_absolute() ? metadata : tree / metadata;
			out.parts.push_back(std::move(part));
		}
		out.paletteParts = json.at("palette_parts").get<std::vector<uint32_t>>();
		for (const uint32_t part : out.paletteParts) {
			if (part >= out.parts.size()) {
				return fail(std::format("'{}' names palette part {} of {}", path.string(), part, out.parts.size()));
			}
		}
		return out;
	}
	catch (const std::exception& e) {
		return fail(std::format("'{}': {}", path.string(), e.what()));
	}
}

}
