#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <BasicRenderer/Assets/ImportSettings.h>
#include <BasicRenderer/Assets/TexturePipeline.h>
#include <BasicRenderer/Assets/ImportedAsset.h>
#include <BasicRenderer/Assets/Import/SceneCache.h>

namespace GlTFLoader {
	// The file's default scene as a payload: one part per mesh node, placed by the node's global matrix within the file.
	// Skinned parts carry base skeletons built from their joint nodes (no scene entities), with the file's animations.
	std::optional<br::import::ImportedAssetPayload> LoadImportedAssetFromFile(const std::string& filePath, const br::import::ImportSettings& settings = {});
	// The texture requests the file's materials make when it loads, one per distinct cache key and without samplers (which
	// need a device): for offline tools to condition, or confirm streamed from their files, exactly what the runtime loads.
	std::vector<br::textures::TextureRequest> BuildMaterialTextureRequests(
		const nlohmann::json& gltf,
		const std::string& filePath,
		const br::import::ImportSettings& settings = {});

	// The file as a scene cache holds it, for CLodCacheTool to write into its scene manifest: the parts of the meshes
	// includeMesh keeps (all when unset), their primitives' CLod identities and formats, and their materials. GPU-free.
	// Throws if a texture would not realize as a live import requests it.
	br::import::SceneCacheDescription DescribeScene(
		const nlohmann::json& gltf,
		const std::string& filePath,
		const br::import::ImportSettings& settings,
		const std::string& sourceIdentifier,
		const std::function<bool(size_t meshIndex, std::string_view meshName)>& includeMesh = {});

	// The texture requests a cached scene's materials make, one per distinct cache key and without samplers: what a
	// tool conditions so the scene's textures are ready.
	std::vector<br::textures::TextureRequest> SceneTextureRequests(
		const br::import::SceneCacheDescription& scene,
		const std::string& sourceLocation,
		const br::import::ImportSettings& settings);

	// A scene from its cache alone: its CLod data from the cache root and its textures from the files they stream from,
	// relative to sourceLocation (where the source is, whether or not it still is). nullopt, with a warning, when
	// requireComplete and any primitive has no current CLod data; otherwise those primitives are left out.
	std::optional<br::import::ImportedAssetPayload> LoadImportedAssetFromSceneCache(
		const br::import::SceneCacheDescription& scene,
		const std::string& sourceLocation,
		const br::import::ImportSettings& settings,
		bool requireComplete);
}
