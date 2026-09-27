#pragma once

#include <string>

namespace CLodCacheLoader {

struct MeshCacheIdentity {
	std::string sourceIdentifier;
	std::string primPath;
	std::string subsetName;
	bool doubleSidedVoxelSourceNormals = false;
};

}
