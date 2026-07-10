#pragma once

#include <optional>
#include <string>
#include <vector>

#include <pxr/usd/usdShade/material.h>

#include "Import/CLodCacheLoader.h"
#include "Materials/MaterialDescription.h"

namespace USDMaterialCache {

inline constexpr std::uint32_t kManifestVersion = 1;

struct AssemblyMaterialEntry {
	CLodCacheLoader::MeshCacheIdentity identity;
	MaterialDescription material;
};

// Extracts the renderer-facing, texture-path-only description. No texture data is
// decoded, making this safe for the headless CLodCacheTool.
MaterialDescription ExtractMaterialDescription(const pxr::UsdShadeMaterial& material);

bool SaveAssemblyMaterialManifest(
	const std::string& sourceIdentifier,
	const std::vector<AssemblyMaterialEntry>& entries);

std::optional<std::vector<AssemblyMaterialEntry>> LoadAssemblyMaterialManifest(
	const std::string& sourceIdentifier);

} // namespace USDMaterialCache
