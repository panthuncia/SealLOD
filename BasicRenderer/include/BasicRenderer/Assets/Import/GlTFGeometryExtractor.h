#pragma once

// Geometry extractor for glTF / GLB files.
// Parses the document, reads vertex/index data, builds CLod caches,
// and returns per-primitive MeshPreprocessResult objects.
//
// This file has NO dependencies on GPU / RHI / Scene / Material types,
// so it can be used both by the renderer and by the headless CLI cache-
// builder tool.

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <BasicRenderer/Assets/Import/MeshPreprocessData.h>

namespace GlTFGeometryExtractor {

struct ExtractedPrimitive {
	size_t meshIndex;
	size_t primitiveIndex;
	MeshPreprocessResult result;

	ExtractedPrimitive(size_t mi, size_t pi, MeshPreprocessResult&& r)
		: meshIndex(mi), primitiveIndex(pi), result(std::move(r)) {}
};

struct ExtractionResult {
	nlohmann::json gltf;
	std::vector<ExtractedPrimitive> primitives;
};

// Parse a glTF/GLB file, extract all geometry, and build/load CLod
// caches for every primitive.  Returns the parsed JSON (for scene-
// hierarchy / material use by the renderer) and per-primitive data.
ExtractionResult ExtractAll(const std::string& filePath);

} // namespace GlTFGeometryExtractor
