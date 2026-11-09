#include <BasicRenderer/Assets/Import/SceneManifest.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/CachePaths.h>

namespace br::import
{
	namespace
	{
		using json = nlohmann::json;
		constexpr std::string_view kManifestSuffix = ".brscene.json";

		std::string Lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		std::string HashText(std::uint64_t value)
		{
			char text[17];
			std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
			return text;
		}
	}

	bool IsSceneManifestPath(const std::filesystem::path& path)
	{
		const std::string name = Lower(path.filename().string());
		return name.size() > kManifestSuffix.size() && name.ends_with(kManifestSuffix);
	}

	std::filesystem::path SceneManifestPathFor(const std::filesystem::path& cacheDirectory, const std::filesystem::path& sourcePath)
	{
		return cacheDirectory / (sourcePath.stem().string() + std::string(kManifestSuffix));
	}

	SceneManifest ReadSceneManifest(const std::filesystem::path& manifestPath)
	{
		std::ifstream stream(manifestPath, std::ios::binary);
		if (!stream) throw std::runtime_error("scene manifest: cannot open '" + manifestPath.string() + "'");
		json document;
		try {
			document = json::parse(std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>()));
		}
		catch (const std::exception& error) {
			throw std::runtime_error("scene manifest '" + manifestPath.string() + "' is not JSON: " + error.what());
		}
		const auto schema = document.value("schema", 0u);
		if (schema != SceneManifest::kSchemaVersion) {
			throw std::runtime_error("scene manifest '" + manifestPath.string() + "' has schema " + std::to_string(schema) +
				", this build reads " + std::to_string(SceneManifest::kSchemaVersion) + "; rebuild it with CLodCacheTool");
		}
		SceneManifest manifest;
		const auto& source = document.at("source");
		manifest.sourcePath = source.value("path", std::string());
		manifest.sourceAbsolutePath = source.value("absolutePath", std::string());
		manifest.sourceIdentifier = source.value("identifier", std::string());
		manifest.cacheRoot = document.value("cacheRoot", std::string("."));
		if (document.contains("profile") && document["profile"].is_object()) {
			manifest.profile = ParseImportProfile(document["profile"].dump());
		}
		if (document.contains("clod")) {
			manifest.clodSchemaVersion = document["clod"].value("schema", 0u);
			manifest.clodBuildConfigHash = std::stoull(document["clod"].value("buildConfigHash", std::string("0")), nullptr, 16);
		}
		if (document.contains("counts") && document["counts"].is_object()) {
			for (const auto& [name, value] : document["counts"].items()) {
				if (value.is_number_unsigned()) manifest.counts.emplace_back(name, value.get<std::uint64_t>());
			}
		}
		manifest.generator = document.value("generator", std::string());
		manifest.created = document.value("created", std::string());
		return manifest;
	}

	void WriteSceneManifest(const std::filesystem::path& manifestPath, const SceneManifest& manifest)
	{
		json document = json::object();
		document["schema"] = SceneManifest::kSchemaVersion;
		document["source"] = {
			{ "path", manifest.sourcePath },
			{ "absolutePath", manifest.sourceAbsolutePath },
			{ "identifier", manifest.sourceIdentifier },
		};
		document["cacheRoot"] = manifest.cacheRoot;
		if (manifest.profile) document["profile"] = json::parse(ImportProfileToJson(*manifest.profile));
		document["clod"] = { { "schema", manifest.clodSchemaVersion }, { "buildConfigHash", HashText(manifest.clodBuildConfigHash) } };
		json counts = json::object();
		for (const auto& [name, value] : manifest.counts) counts[name] = value;
		document["counts"] = std::move(counts);
		document["generator"] = manifest.generator;
		document["created"] = manifest.created;

		std::filesystem::create_directories(manifestPath.parent_path());
		const auto temporary = std::filesystem::path(manifestPath).concat(".tmp");
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			if (!stream) throw std::runtime_error("scene manifest: cannot write '" + temporary.string() + "'");
			stream << document.dump(2) << '\n';
			if (!stream) throw std::runtime_error("scene manifest: write failed for '" + temporary.string() + "'");
		}
		std::filesystem::rename(temporary, manifestPath);
	}

	std::filesystem::path ResolveSceneManifestSource(const std::filesystem::path& manifestPath, const SceneManifest& manifest)
	{
		std::error_code error;
		if (!manifest.sourcePath.empty()) {
			const auto relative = (manifestPath.parent_path() / std::filesystem::path(manifest.sourcePath)).lexically_normal();
			if (std::filesystem::is_regular_file(relative, error)) return relative;
		}
		if (!manifest.sourceAbsolutePath.empty() && std::filesystem::is_regular_file(manifest.sourceAbsolutePath, error)) {
			return std::filesystem::path(manifest.sourceAbsolutePath);
		}
		throw std::runtime_error("scene manifest '" + manifestPath.string() + "': its source is at neither '" + manifest.sourcePath +
			"' (relative to the manifest) nor '" + manifest.sourceAbsolutePath + "'");
	}

	std::filesystem::path ResolveSceneManifestCacheRoot(const std::filesystem::path& manifestPath, const SceneManifest& manifest)
	{
		return std::filesystem::absolute(manifestPath.parent_path() / std::filesystem::path(manifest.cacheRoot)).lexically_normal();
	}

	bool ApplySceneManifestCacheRoot(const std::filesystem::path& manifestPath)
	{
		const SceneManifest manifest = ReadSceneManifest(manifestPath);
		const auto root = ResolveSceneManifestCacheRoot(manifestPath, manifest);
		if (SetCacheRootPath(root)) {
			spdlog::info("Scene manifest '{}': cache root {}", manifestPath.string(), root.string());
			return true;
		}
		spdlog::warn("Scene manifest '{}' wants cache root {}, but {} is already in use; its CLod data will not be found",
			manifestPath.string(), root.string(), GetCacheRootPath().string());
		return false;
	}
}
