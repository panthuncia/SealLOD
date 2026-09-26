#pragma once

// Geometry extractor for Assimp-loaded assets.
// Extracts vertex/index data, builds CLod caches, and returns
// per-mesh MeshPreprocessResult objects.
//
// No GPU / RHI / Scene / Material dependencies.

#include <cstddef>
#include <string>
#include <vector>

#include <BasicRenderer/Assets/Import/MeshPreprocessData.h>

struct aiScene;

namespace AssimpGeometryExtractor {

struct ExtractedMesh {
	unsigned int meshIndex;
	unsigned int materialIndex;
	bool hasBones;
	MeshPreprocessResult result;

	ExtractedMesh(unsigned int mi, unsigned int matIdx, bool bones, MeshPreprocessResult&& r)
		: meshIndex(mi), materialIndex(matIdx), hasBones(bones), result(std::move(r)) {}
};

struct ExtractionResult {
	std::vector<ExtractedMesh> meshes;
};

// Extract from an already-loaded aiScene (for renderer use).
ExtractionResult ExtractAll(const aiScene* scene, const std::string& sourceFilePath);

// Convenience: load file via Assimp + extract (for CLI tool).
ExtractionResult ExtractAll(const std::string& filePath);

} // namespace AssimpGeometryExtractor
