#pragma once

// The parts a hierarchy root's portals open (docs/merge_hierarchies.md in SARP), written beside the root's CLod cache.
//
// A root's cache knows its parts only by ordinal (ClusterLODAssemblyInstance::stackDepth). Whoever builds the root
// knows which registered meshes those are; this file records their caches so a tool can open a root and its parts
// with nothing but the cache tree. The renderer never reads it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <BasicRenderer/Assets/Import/CLodCache.h>

namespace CLodCache {

inline constexpr uint32_t kPortalPartsVersion = 1;

struct PortalPart {
	CacheKey key;
	uint64_t buildConfigHash = 0;
	std::string label;  // for people: what the builder called it
	std::filesystem::path metadataPath;  // filled by LoadPortalParts, from the cache tree the root is in
};

struct PortalParts {
	std::vector<PortalPart> parts;  // in portal ordinal order
	std::vector<uint32_t> paletteParts;  // palette slot -> the part whose material it is
};

// "<root metadata>.clodparts", beside the root's .clodmeta.
std::filesystem::path PortalPartsPath(const std::filesystem::path& rootMetadataPath);

// Writes the file for the root saved under (rootKey, rootBuildConfigHash), with the parts' metadata paths relative to
// the cache tree, so the tree can move.
bool SavePortalParts(const CacheKey& rootKey, uint64_t rootBuildConfigHash, const PortalParts& parts);

// Reads the file beside a root's metadata, resolving each part's metadata path against the same cache tree. nullopt
// when there is no file (the mesh is not a root, or its builder wrote none) or it cannot be read.
std::optional<PortalParts> LoadPortalParts(const std::filesystem::path& rootMetadataPath, std::string* outError = nullptr);

}
