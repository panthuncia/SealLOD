#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>

#include <optional>
#include <string>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/usd/attribute.h>

#include <BasicRenderer/Assets/CachePaths.h>

// The USD half of cache identity. Kept apart so the cache store has no USD dependency.
namespace CLodCacheLoader {

namespace {
	struct LayerPrimIdentity {
		std::string sourceIdentifier;
		std::string primPath;
	};

	std::string GetLayerCacheSourceIdentifier(const pxr::SdfLayerHandle& layer)
	{
		if (!layer) {
			return {};
		}

		const std::string& realPath = layer->GetRealPath();
		return NormalizeCacheSourcePath(realPath.empty() ? layer->GetIdentifier() : realPath);
	}

	std::optional<LayerPrimIdentity> GetStrongestAuthoredValueIdentity(
		const pxr::UsdAttribute& attr,
		pxr::UsdTimeCode geomTimeCode)
	{
		if (!attr) {
			return std::nullopt;
		}

		for (const auto& spec : attr.GetPropertyStack(geomTimeCode)) {
			if (!spec || !spec->GetLayer()) {
				continue;
			}

			const bool hasTimeSamples =
				!spec->GetLayer()->ListTimeSamplesForPath(spec->GetPath()).empty();
			if (!spec->HasDefaultValue() && !hasTimeSamples) {
				continue;
			}

			LayerPrimIdentity identity{};
			identity.sourceIdentifier = GetLayerCacheSourceIdentifier(spec->GetLayer());
			identity.primPath = spec->GetPath().GetPrimPath().GetString();
			if (!identity.sourceIdentifier.empty() && !identity.primPath.empty()) {
				return identity;
			}
		}

		return std::nullopt;
	}

	std::optional<LayerPrimIdentity> GetReferencedGeometryIdentity(
		const pxr::UsdGeomMesh& mesh,
		pxr::UsdTimeCode geomTimeCode)
	{
		const std::optional<LayerPrimIdentity> pointsIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetPointsAttr(), geomTimeCode);
		const std::optional<LayerPrimIdentity> countsIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetFaceVertexCountsAttr(), geomTimeCode);
		const std::optional<LayerPrimIdentity> indicesIdentity =
			GetStrongestAuthoredValueIdentity(mesh.GetFaceVertexIndicesAttr(), geomTimeCode);

		if (!pointsIdentity || !countsIdentity || !indicesIdentity) {
			return std::nullopt;
		}

		const auto matchesPoints = [&pointsIdentity](const LayerPrimIdentity& other) {
			return other.sourceIdentifier == pointsIdentity->sourceIdentifier &&
				other.primPath == pointsIdentity->primPath;
		};

		if (!matchesPoints(*countsIdentity) || !matchesPoints(*indicesIdentity)) {
			return std::nullopt;
		}

		return pointsIdentity;
	}
}

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode)
{
	MeshCacheIdentity identity{};
	if (stage && stage->GetRootLayer()) {
		identity.sourceIdentifier = NormalizeCacheSourcePath(
			stage->GetRootLayer()->GetIdentifier());
	}
	identity.primPath = mesh.GetPrim().GetPath().GetString();
	identity.subsetName = subsetName;

	if (auto referencedGeometryIdentity = GetReferencedGeometryIdentity(mesh, geomTimeCode)) {
		identity.sourceIdentifier = std::move(referencedGeometryIdentity->sourceIdentifier);
		identity.primPath = std::move(referencedGeometryIdentity->primPath);
	}

	return identity;
}

MeshCacheIdentity BuildIdentity(
	const pxr::UsdGeomMesh& mesh,
	const pxr::UsdStageRefPtr& stage,
	const std::string& subsetName,
	pxr::UsdTimeCode geomTimeCode,
	const std::string& sourceIdentifierOverride)
{
	MeshCacheIdentity identity = BuildIdentity(mesh, stage, subsetName, geomTimeCode);
	if (!sourceIdentifierOverride.empty()) {
		identity.sourceIdentifier = NormalizeCacheSourcePath(sourceIdentifierOverride);
	}
	return identity;
}

}
