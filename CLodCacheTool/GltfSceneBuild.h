#pragma once

// A glTF scene's cache: every primitive's CLod data, its material textures conditioned or confirmed to stream straight
// from their files, and the scene manifest that opens it (br/Assets/Import/SceneManifest.h).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <BasicRenderer/Assets/Import/ImportProfile.h>
#include <BasicRenderer/Assets/Import/MeshPreprocessData.h>

struct GltfSceneBuildOptions
{
	// The cache root, where the manifest goes.
	std::filesystem::path outDirectory;
	std::shared_ptr<const br::import::ImportProfile> profile;
	// ECMAScript regexes over mesh names: only meshes matching onlyMeshes (when set), none matching skipMeshes.
	std::string onlyMeshes;
	std::string skipMeshes;
	// Meshes whose index % shardCount == shardIndex.
	std::uint32_t shardIndex = 0;
	std::uint32_t shardCount = 1;
	// Primitives building at once are bounded by triangles * bytesPerTriangle against this; 0 picks half of physical
	// memory.
	std::uint64_t memoryBudgetBytes = 0;
	std::uint64_t bytesPerTriangle = 1024;
	unsigned jobs = 0;
	// Report misses instead of building, and leave the manifest as it is.
	bool cacheOnly = false;
	bool conditionTextures = true;
	// Each primitive's built or loaded CLod data, for reports. Called from workers.
	std::function<void(const std::string& label, const MeshPreprocessResult& result)> report;
};

// True when every selected primitive is cached and every material texture is either cached or streams from its file.
bool BuildGltfScene(const std::filesystem::path& scenePath, const GltfSceneBuildOptions& options);
