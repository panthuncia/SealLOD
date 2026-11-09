#pragma once

// A scene cache's manifest, <cache dir>/<scene>.brscene.json: what CLodCacheTool prepared and how to open it. Loading a
// manifest (br::import::LoadImportedAsset) points the cache root at the manifest's directory, applies the import profile
// it recorded, and imports the source scene, whose CLod data is then all cache hits. Files the scene references by
// path (its textures) are read where they are; the cache holds only what had to be built.
//
// {
//   "schema": 1,
//   "source": { "path": "../zorah/zorah.gltf", "absolutePath": "D:/Models/zorah/zorah.gltf",
//               "identifier": "gltf://zorah.gltf#0123456789abcdef" },
//   "cacheRoot": ".",
//   "profile": { ...ImportProfile... },
//   "clod": { "schema": 81, "buildConfigHash": "..." },
//   "counts": { "primitives": 7277, ... },
//   "generator": "CLodCacheTool", "created": "2026-11-09T01:23:45Z"
// }

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <BasicRenderer/Assets/Import/ImportProfile.h>

namespace br::import
{
	struct SceneManifest
	{
		static constexpr std::uint32_t kSchemaVersion = 1;

		// The source scene: relative to the manifest's directory when it can be, and absolute as a fallback.
		std::string sourcePath;
		std::string sourceAbsolutePath;
		// The source's CLod cache identity when the cache was built (glTF: "gltf://<file>#<content hash>").
		std::string sourceIdentifier;
		// The cache root, relative to the manifest's directory.
		std::string cacheRoot = ".";
		std::optional<ImportProfile> profile;
		std::uint32_t clodSchemaVersion = 0;
		std::uint64_t clodBuildConfigHash = 0;
		// What the build did, for people: name -> count.
		std::vector<std::pair<std::string, std::uint64_t>> counts;
		std::string generator;
		std::string created;
	};

	// Whether a path names a scene manifest (*.brscene.json).
	[[nodiscard]] bool IsSceneManifestPath(const std::filesystem::path& path);
	// The manifest path CLodCacheTool writes for a source scene into a cache directory.
	[[nodiscard]] std::filesystem::path SceneManifestPathFor(const std::filesystem::path& cacheDirectory, const std::filesystem::path& sourcePath);

	// Throws std::runtime_error naming what is wrong.
	[[nodiscard]] SceneManifest ReadSceneManifest(const std::filesystem::path& manifestPath);
	// Writes through a temporary file, so a reader never sees half a manifest. Throws on failure.
	void WriteSceneManifest(const std::filesystem::path& manifestPath, const SceneManifest& manifest);

	// The source scene's file: the relative path if it exists, else the absolute one. Throws if neither exists.
	[[nodiscard]] std::filesystem::path ResolveSceneManifestSource(const std::filesystem::path& manifestPath, const SceneManifest& manifest);
	[[nodiscard]] std::filesystem::path ResolveSceneManifestCacheRoot(const std::filesystem::path& manifestPath, const SceneManifest& manifest);
	// Points the process's cache root at a manifest's cache. Call before anything resolves the cache root (the renderer's
	// startup does); false, with a warning logged, if a different root is already in use.
	bool ApplySceneManifestCacheRoot(const std::filesystem::path& manifestPath);
}
