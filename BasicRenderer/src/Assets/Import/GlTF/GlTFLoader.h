#pragma once

#include <memory>
#include <optional>
#include <string>

#include <BasicRenderer/Assets/ImportedAsset.h>

namespace GlTFLoader {
	// The file's default scene as a payload: one part per mesh node, placed by the node's global matrix within the file.
	// Skinned parts carry base skeletons built from their joint nodes (no scene entities), with the file's animations.
	std::optional<br::import::ImportedAssetPayload> LoadImportedAssetFromFile(const std::string& filePath);
}
