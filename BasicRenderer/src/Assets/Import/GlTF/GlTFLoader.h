#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <BasicRenderer/Assets/ImportSettings.h>
#include <BasicRenderer/Assets/TexturePipeline.h>
#include <BasicRenderer/Assets/ImportedAsset.h>

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
}
