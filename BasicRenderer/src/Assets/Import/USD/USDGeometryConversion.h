#pragma once

#include "Assets/Import/USD/USDImportState.h"
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include <pxr/usd/usdSkel/cache.h>

namespace USDLoader {

bool IsUnsupportedBrNiflySkinnedMesh(const pxr::UsdGeomMesh& mesh);
bool IsBrNiflyCollisionMesh(const pxr::UsdGeomMesh& mesh);
bool IsBrNiflyObjectRootPrim(const pxr::UsdPrim& prim);
bool IsBrNiflyLODRenderMesh(const pxr::UsdGeomMesh& mesh);
std::shared_ptr<Material> ResolveMaterialForMesh(
    const pxr::UsdShadeMaterial& material, const std::vector<MeshUvSetData>& uvSets,
    bool forceDoubleSided = false, const pxr::UsdPrim& meshPrim = pxr::UsdPrim(),
    const MeshPreprocessResult* preprocessResult = nullptr,
    std::string_view staticTextureOverrideSourceName = {});
USDGeometryExtractor::ExtractOptions BuildGeometryExtractOptions(
    const pxr::UsdGeomMesh& mesh, const pxr::UsdShadeMaterial& material,
    const InMemoryStageOptions& stageOptions);
bool ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(
    const USDGeometryExtractor::ExtractOptions& options);

std::optional<CLodCacheLoader::MeshCacheIdentity> BuildPointInstancerAssemblyIdentity(
    const pxr::UsdStageRefPtr& stage, const std::string& sourceIdentifier, pxr::UsdTimeCode geomTimeCode);
void TryLoadPointInstancerAssemblyMesh(
    const pxr::UsdStageRefPtr& stage, pxr::UsdTimeCode geomTimeCode, const std::string& sourceIdentifier);
void PreprocessAllMeshes(
    const pxr::UsdStageRefPtr& stage, double metersPerUnit, const std::string& directory,
    bool isUSDZ, const ImportSettings& importSettings, const InMemoryStageOptions& stageOptions,
    const std::string& sourceIdentifierOverride = {}, bool retainArtifactsForAssetAssembly = false);
std::vector<std::shared_ptr<Mesh>> ProcessMesh(
    const pxr::UsdGeomMesh& mesh, const pxr::UsdStageRefPtr& stage, double metersPerUnit,
    pxr::GfRotation upRot, const std::string& directory, bool isUSDZ,
    const pxr::UsdSkelCache& skelCache, pxr::VtTokenArray& skelJointOrderRaw,
    pxr::VtTokenArray& skelJointOrderMapped);

}
