#pragma once

#include <optional>
#include <string>

#include <BasicRenderer/Assets/CLodCacheIdentity.h>
#include <BasicRenderer/Assets/CLodCacheStore.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdGeom/mesh.h>

#include <BasicRenderer/Assets/ClusterLODTypes.h>

namespace CLodCacheLoader {

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode = pxr::UsdTimeCode::Default());

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode,
	const std::string& sourceIdentifierOverride);

}
