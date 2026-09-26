#pragma once

#include "Assets/Import/USD/USDImportState.h"
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include <boost/functional/hash.hpp>
#include <pxr/usd/usdSkel/cache.h>

namespace USDLoader {
using namespace pxr;

	struct StageImportContext {
		double metersPerUnit = 1.0;
		GfRotation upRot;
		std::string directory;
		bool isUSDZ = false;
	};

	struct AssetAssemblyBucketKey
	{
		bool skinned = false;
		std::uint64_t skinDomain = 0u;
		std::string materialPath;

		bool operator==(const AssetAssemblyBucketKey& other) const noexcept
		{
			return skinned == other.skinned &&
				skinDomain == other.skinDomain &&
				materialPath == other.materialPath;
		}
	};

	struct AssetAssemblyBucketKeyHash
	{
		std::size_t operator()(const AssetAssemblyBucketKey& key) const noexcept
		{
			std::size_t result = static_cast<std::size_t>(key.skinDomain ^ (key.skinned ? 0x9E3779B97F4A7C15ull : 0ull));
			boost::hash_combine(result, key.materialPath);
			return result;
		}
	};

	struct AssetAssemblyBucketInfo
	{
		AssetAssemblyBucketKey key;
		std::shared_ptr<Skeleton> skeleton;
		UsdShadeMaterial material;
		std::string staticTextureOverrideSourceName;
		std::vector<std::string> meshPaths;
		std::vector<GfMatrix4d> skeletonInstanceTransforms;
		UsdGeomMesh firstMesh;
		bool forceDoubleSided = false;
	};

std::optional<CLodCacheLoader::MeshCacheIdentity> BuildAssetAssemblyIdentity(
    const UsdStageRefPtr& stage, const std::string& sourceIdentifier,
    UsdTimeCode geomTimeCode, const AssetAssemblyBucketInfo& bucket);
std::shared_ptr<Mesh> BuildMeshFromAssetAssemblyPrebuilt(
    std::optional<ClusterLODPrebuiltData>&& prebuilt,
    const std::shared_ptr<Material>& material);


std::vector<AssetAssemblyBucketInfo> DiscoverAssetAssemblyBuckets(
    const UsdStageRefPtr& stage, UsdSkelCache& skelCache, double metersPerUnit,
    UsdTimeCode geomTimeCode, const InMemoryStageOptions& stageOptions,
    const ImportSettings& importSettings, std::string* fallbackReason = nullptr);
bool TryLoadAssetAssemblyMeshes(
    const UsdStageRefPtr& stage, const StageImportContext& stageContext,
    const ImportSettings& importSettings, const InMemoryStageOptions& options,
    const std::string& sourceIdentifier, UsdTimeCode geomTimeCode,
    const std::vector<AssetAssemblyBucketInfo>& buckets,
    std::vector<std::shared_ptr<Mesh>>& meshes);
bool BuildAssetAssemblyMeshesFromPreprocessedData(
    const UsdStageRefPtr& stage, const StageImportContext& stageContext,
    const ImportSettings& importSettings, const InMemoryStageOptions& options,
    const std::string& sourceIdentifier, UsdTimeCode geomTimeCode,
    const std::vector<AssetAssemblyBucketInfo>& buckets,
    std::vector<std::shared_ptr<Mesh>>& meshes, std::string* outFailureReason = nullptr);
std::shared_ptr<Scene> CreateCollapsedAssetAssemblyScene(
    const std::vector<std::shared_ptr<Mesh>>& meshes,
    const std::vector<AssetAssemblyBucketInfo>& buckets,
    const GfRotation& upAxisCorrection);

}
