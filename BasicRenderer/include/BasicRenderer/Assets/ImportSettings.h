#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace USDLoader {

struct ImportSettings {
	bool enableDoubleSidedNameHeuristic = true;
	bool loadMaterialTextures = true;
	// BRNifly exposes Skyrim NIF skin metadata without authoring a UsdSkelSkeleton.
	// TREE imports opt into synthesizing that metadata into a procedural-wind
	// skeleton; ordinary uses of the same NIF remain static.
	bool enableNifTreeProceduralWind = false;
	std::uint32_t nifTessellationFactor = 1;
	std::vector<std::string> additionalTextureSearchRoots;
	// Optional VFS/archive resolver used by headless importers. The returned
	// path must name a materialized filesystem file for the duration of the
	// import. Loose-file and ordinary USD resolution remain the first choice.
	std::function<std::optional<std::string>(std::string_view)> resolveResourcePath;
	// Offline-only. Allows construction of the temporary geometry/material
	// recipe used to publish Reyes height-atlas variants.
	bool prepareObjectReyesAtlasRecipes = false;
};

} // namespace USDLoader

namespace br::import {
using ImportSettings = USDLoader::ImportSettings;
} // namespace br::import
