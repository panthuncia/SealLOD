#pragma once

// Geometry extractor for glTF / GLB files.
// Parses the document, reads vertex/index data, builds CLod caches,
// and returns per-primitive MeshPreprocessResult objects.
//
// This file has NO dependencies on GPU / RHI / Scene / Material types,
// so it can be used both by the renderer and by the headless CLI cache-
// builder tool.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
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
	// The CLod cache identity of the file: "gltf://<file name>#<content hash>". It names the file's content, not its
	// place, so a scene and its cache can each be moved or copied.
	std::string sourceIdentifier;
	// Most triangles admitted at once under a memory budget: with the peak working set, what a triangle really costs.
	std::uint64_t peakAdmittedTriangles = 0;
};

// What happened to one primitive.
struct PrimitiveOutcome {
	enum class Kind : std::uint8_t {
		Hit,      // its CLod cache was current
		Built,    // built and saved
		Missing,  // cache-only and not cached
		Failed,   // threw; error says why
	};
	size_t meshIndex = 0;
	size_t primitiveIndex = 0;
	Kind kind = Kind::Hit;
	std::uint64_t triangles = 0;
	double seconds = 0.0;
	std::string error;
};

struct ExtractOptions {
	// Meshes to leave out entirely, by index and name. Unset: every mesh.
	std::function<bool(size_t meshIndex, std::string_view meshName)> skipMesh;
	// Each primitive as soon as it is ready, on the worker that made it, instead of collecting them in the result. For
	// tools that only want the cache written: a scene's prebuilt data need never be held all at once.
	std::function<void(ExtractedPrimitive&&)> onPrimitive;
	// After each primitive, from the worker that ran it.
	std::function<void(const PrimitiveOutcome&)> onOutcome;
	// A cache miss is reported as Missing instead of building.
	bool cacheOnly = false;
	// A primitive that throws is reported as Failed and the rest go on; otherwise the first failure is rethrown.
	bool continueOnError = false;
	// Bounds the memory of primitives building at once: each is admitted at triangles * bytesPerTriangle, largest first,
	// and one always runs. 0: no bound (the scheduler's ParallelFor).
	std::uint64_t memoryBudgetBytes = 0;
	std::uint64_t bytesPerTriangle = 320;
	// Most primitives admitted at once when memoryBudgetBytes bounds them. 0: only the budget limits them.
	unsigned jobs = 0;
	// The identity a cache was built for; a file now hashing differently is reported (its caches will miss).
	std::string expectedSourceIdentifier;
};

// Parse a glTF/GLB file, extract all geometry, and build/load CLod
// caches for every primitive.  Returns the parsed JSON (for scene-
// hierarchy / material use by the renderer) and per-primitive data.
ExtractionResult ExtractAll(const std::string& filePath, const ExtractOptions& options = {});

// A primitive's CLod cache identity and the vertex format its mesh is made with, from the document alone: what a
// cached scene records so its meshes are made exactly as an import of the source makes them.
struct PrimitiveFormat {
	CLodCacheLoader::MeshCacheIdentity identity;
	unsigned int vertexFlags = 0;
	unsigned int vertexSize = 0;
	unsigned int skinningVertexSize = 0;
	std::uint32_t uvSetCount = 0;
};
PrimitiveFormat DescribePrimitiveFormat(const nlohmann::json& gltf, const std::string& sourceIdentifier, size_t meshIndex, size_t primitiveIndex);

// Required extensions this importer reads. A file requiring any other is refused.
bool IsSupportedRequiredExtension(std::string_view extension);

} // namespace GlTFGeometryExtractor
