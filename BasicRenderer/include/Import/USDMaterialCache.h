#pragma once

#include <optional>
#include <string>
#include <vector>

#include <pxr/usd/usdShade/material.h>

#include "Import/CLodCacheLoader.h"
#include "Materials/MaterialDescription.h"

namespace USDMaterialCache {

// Version 6 invalidates manifests that can replay pre-skinning-aware CLod
// assembly identities. The manifest fast path bypasses USD extraction, so its
// version must advance whenever the assembly payload ABI advances.
inline constexpr std::uint32_t kManifestVersion = 6;

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

void RemoveAssemblyMaterialManifest(const std::string& sourceIdentifier);

std::optional<std::vector<AssemblyMaterialEntry>> LoadAssemblyMaterialManifest(
	const std::string& sourceIdentifier);

} // namespace USDMaterialCache
