#pragma once

#include <optional>
#include <string>

#include <BasicRenderer/Assets/ImportedAsset.h>
#include <BasicRenderer/Assets/ImportSettings.h>

namespace br::import {
// A model file as a payload, by extension: glTF/GLB, USD (.usd/.usda/.usdc/.usdz) or NIF. The file's own path is its
// source identity (caches, NIF conversion). Empty on failure or for a format with no payload loader.
std::optional<ImportedAssetPayload> LoadImportedAsset(const std::string& filePath, const ImportSettings& settings = {});
}
