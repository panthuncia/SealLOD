#pragma once

// A scene cache's manifest, <cache dir>/<scene>.brscene.json: what CLodCacheTool prepared and how to open it. Loading a
// manifest (br::import::LoadImportedAsset) points the cache root at the manifest's directory and applies the import
// profile it recorded. With a "scene" section (SceneCache.h) the scene loads from the cache alone, without opening the
// source; without one, or when its CLod data is incomplete, the source scene is imported and its CLod data is then all
// cache hits. Files the scene references by path (its textures) are read where they are; the cache holds only what had
// to be built.
//
// {
//   "schema": 2,
//   "source": { "path": "../zorah/zorah.gltf", "absolutePath": "D:/Models/zorah/zorah.gltf",
//               "identifier": "gltf://zorah.gltf#0123456789abcdef" },
//   "cacheRoot": ".",
//   "profile": { ...ImportProfile... },
//   "clod": { "schema": 81, "buildConfigHash": "..." },
//   "counts": { "primitives": 7277, ... },
//   "generator": "CLodCacheTool", "created": "2026-11-09T01:23:45Z",
//   "scene": { ...SceneCacheDescription... }
// }

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <BasicRenderer/Assets/Import/ImportProfile.h>
#include <BasicRenderer/Assets/Import/SceneCache.h>

namespace br::import
{
	struct SceneManifest
	{
		// 2 added "scene"; schema 1 manifests still read.
		static constexpr std::uint32_t kSchemaVersion = 2;

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
		// The scene as the cache holds it, to load without the source.
		std::optional<SceneCacheDescription> scene;
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
	// Where the source scene is, whether or not it exists: what a cached scene's relative texture paths resolve against.
	[[nodiscard]] std::filesystem::path SceneManifestSourceLocation(const std::filesystem::path& manifestPath, const SceneManifest& manifest);
	// The scene manifests directly in a directory, sorted by name.
	[[nodiscard]] std::vector<std::filesystem::path> FindSceneManifests(const std::filesystem::path& directory);
	[[nodiscard]] std::filesystem::path ResolveSceneManifestCacheRoot(const std::filesystem::path& manifestPath, const SceneManifest& manifest);
	// Points the process's cache root at a manifest's cache. Call before anything resolves the cache root (the renderer's
	// startup does); false, with a warning logged, if a different root is already in use.
	bool ApplySceneManifestCacheRoot(const std::filesystem::path& manifestPath);
}
