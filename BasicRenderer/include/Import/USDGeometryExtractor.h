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
#include <pxr/base/gf/matrix4d.h>

#include "Import/MeshPreprocessData.h"
#include "Materials/MaterialDescription.h"

namespace USDGeometryExtractor {

struct AssemblyMeshInstance {
	pxr::UsdGeomMesh mesh;
	pxr::GfMatrix4d localToStage{ 1.0 };
	// Authored Nanite assembly joint for this PointInstancer element. Empty for
	// ordinary meshes and assets which do not provide assembly binding metadata.
	std::string assemblyBindJoint;
};

// Enumerate visible root meshes and expanded PointInstancer prototype meshes
// in one canonical order and transform space. Both offline and runtime assembly
// builders consume this to avoid drifting USD traversal/composition behavior.
std::vector<AssemblyMeshInstance> EnumerateAssemblyMeshInstances(
	const pxr::UsdStageRefPtr& stage,
	pxr::UsdTimeCode geomTimeCode);

ClusterLODAssemblyTransform AssemblyTransformFromUsdMatrix(
	const pxr::GfMatrix4d& matrix,
	double metersPerUnit);

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
	bool geometricDisplacementOptIn = false;
	ObjectSurfaceSamplingMode objectSurfaceSamplingMode = ObjectSurfaceSamplingMode::None;
	bool objectSurfaceUseTriplanarProjection = false;
	bool objectSurfaceUseTripleTapStochastic = false;
	std::string objectSurfaceSamplingConfigHash;
	bool retainClusterLODArtifacts = false;
	bool skipCachedClusterLODMeshBuilds = false;
	bool buildPointInstancerAssemblyCaches = false;
	bool buildWholeAssetAssemblyCaches = false;
	bool importSkinningAsRigidBindPose = false;
	bool enableDoubleSidedNameHeuristic = true;
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

MeshPreprocessResult ExtractSubMeshGroup(
	const pxr::UsdGeomMesh& mesh,
	const std::vector<pxr::UsdGeomSubset>& subsets,
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
	std::uint32_t tessellationFactor = 1,
	const ExtractOptions& options = {});

// Build and persist the synthetic CLod assembly used to represent all
// PointInstancers in a stage. The supplied submeshes must retain their
// transient CLod artifacts. This lets runtime importers reuse preprocessing
// they have already performed instead of expanding PointInstancers on CPU.
std::optional<MeshPreprocessResult> BuildInstancedAssemblyCache(
	const pxr::UsdStageRefPtr& stage,
	const std::string& sourceIdentifier,
	pxr::UsdTimeCode geomTimeCode,
	const std::vector<const MeshPreprocessResult*>& preprocessedSubmeshes,
	const std::string& identitySuffix = {});

std::optional<CLodCacheLoader::MeshCacheIdentity> BuildWholeAssetAssemblyIdentity(
	const pxr::UsdStageRefPtr& stage,
	const std::string& sourceIdentifier,
	pxr::UsdTimeCode geomTimeCode,
	const pxr::UsdGeomMesh& identityMesh);

std::string BuildWholeAssetAssemblyBucketIdentitySuffix(
	bool skinned,
	std::uint64_t skinDomain,
	const std::string& materialPath);

void AppendWholeAssetAssemblyBucketIdentity(
	CLodCacheLoader::MeshCacheIdentity& identity,
	bool skinned,
	std::uint64_t skinDomain,
	const std::string& materialPath);

} // namespace USDGeometryExtractor
