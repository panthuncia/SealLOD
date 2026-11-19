#pragma once

#include <cstdint>
#include <string>

namespace CLodCacheLoader {

struct MeshCacheIdentity {
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	bool doubleSidedVoxelSourceNormals = false;
	// Stage 6 (docs/clod-appearance-stage6.md): a hash of the material appearance and image content the build weighs texture
	// damage by (ClusterLODMaterialAppearance::contentHash). Importers set it only when that is enabled
	// (ClusterLODMaterialAppearanceWanted); 0 leaves the cache key as it always was.
	std::uint64_t appearanceHash = 0;
};

}
