#pragma once

#include <optional>
#include <cstdint>
#include <string>
#include <vector>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/tf/token.h>

#include "Import/MeshPreprocessData.h"

namespace USDGeometryExtractor {

struct BenchmarkStats {
	std::uint64_t submeshes = 0;
	std::uint64_t clodCacheHits = 0;
	std::uint64_t clodCacheMisses = 0;
	std::uint64_t loadGeomMs = 0;
	std::uint64_t clodBuildMs = 0;
	std::uint64_t clodSaveMs = 0;
	std::uint64_t clodReloadMs = 0;
};

struct ExtractOptions {
	std::optional<float> vertexAlphaCutoff;
	bool brniflyVertexAlpha = false;
	bool brniflyZBufferWrite = true;
	bool brniflyDecal = false;
	bool brniflyDynamicDecal = false;
	bool brniflyModelSpaceNormals = false;
};

void ResetBenchmarkStats();
BenchmarkStats GetBenchmarkStats();

// Extract geometry for a single mesh (or mesh+subset), populate
// MeshIngestBuilder, and build/load CLod cache.
MeshPreprocessResult ExtractSubMesh(
	const pxr::UsdGeomMesh& mesh,
	const std::optional<pxr::UsdGeomSubset>& subset,
	const pxr::UsdStageRefPtr& stage,
	pxr::UsdTimeCode geomTimeCode,
	double metersPerUnit,
	const std::vector<std::string>& requiredUvSetNames,
	const std::optional<pxr::UsdSkelSkinningQuery>& skinQ,
	const pxr::VtTokenArray& skelJointOrderRaw,
	const pxr::VtTokenArray& skelJointOrderMapped,
	bool doubleSidedVoxelSourceNormals = false,
	const std::string& sourceIdentifierOverride = {},
	std::uint32_t tessellationFactor = 1,
	const ExtractOptions& options = {});

// Build a UsdSkelSkinningQuery for a mesh if it has skinning data.
std::optional<pxr::UsdSkelSkinningQuery> GetSkinningQuery(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdSkelCache& skelCache);

// Results from ExtractAll (CLI stage-wide extraction).
struct StageExtractionResult {
	size_t meshesProcessed = 0;
	size_t submeshesProcessed = 0;
	size_t cachesBuilt = 0;
	std::vector<MeshPreprocessResult> submeshes;
};

// Open a USD stage and extract geometry + build CLod caches for every mesh.
StageExtractionResult ExtractAll(const std::string& filePath);
StageExtractionResult ExtractAllFromStage(
	const pxr::UsdStageRefPtr& stage,
	const std::string& sourceIdentifier = {},
	std::uint32_t tessellationFactor = 1);

} // namespace USDGeometryExtractor
