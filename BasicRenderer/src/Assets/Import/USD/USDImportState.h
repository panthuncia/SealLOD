#pragma once

#include <BasicRenderer/Assets/USD/USDLoader.h>
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/Import/MeshPreprocessData.h>
#include <BasicRenderer/Scene/Animation/DynamicWindMetadata.h>
#include <BasicRenderer/Assets/Texture.h>
#include <flecs.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdSkel/skinningQuery.h>
#include <pxr/usd/usdShade/material.h>
#include <unordered_set>

namespace USDLoader {
    using namespace pxr;
    struct MaterialTemplateRecord {
        MaterialDescription desc;
        std::vector<std::string> referencedUvSetNames;
    };

	struct PreprocessedMeshSubset {
		UsdShadeMaterial material;
		std::string staticTextureOverrideSourceName;
		MeshPreprocessResult result;
		bool inferredDoubleSided = false;

		PreprocessedMeshSubset(UsdShadeMaterial m, MeshPreprocessResult&& r, bool inferred)
			: material(std::move(m)), result(std::move(r)), inferredDoubleSided(inferred) {}

		PreprocessedMeshSubset(UsdShadeMaterial m, MeshPreprocessResult&& r, bool inferred, std::string sourceName)
			: material(std::move(m))
			, staticTextureOverrideSourceName(std::move(sourceName))
			, result(std::move(r))
			, inferredDoubleSided(inferred) {}

	};

	struct PreprocessedMeshRecord {
		bool authoredDoubleSided = false;
		std::vector<PreprocessedMeshSubset> subsets;
	};

	struct MeshPreprocessWorkItem {
		std::string meshPath;
		UsdGeomMesh mesh;
		std::vector<UsdGeomSubset> subsets;
		UsdShadeMaterial material;
		std::vector<std::string> requiredUvSetNames;
		std::optional<UsdSkelSkinningQuery> skinQ;
		VtTokenArray skelJointOrderRaw;
		VtTokenArray skelJointOrderMapped;
		USDGeometryExtractor::ExtractOptions extractOptions;
		bool authoredDoubleSided = false;
		bool inferredDoubleSided = false;
	};

	struct ObjectReyesAtlasBakedSubsetResult {
		std::size_t sourceWorkIndex = 0;
		MeshPreprocessResult result;
	};

	struct PayloadSkeletonBuildMetadata
	{
		std::string skeletonPath;
		std::vector<std::string> boneNames;
		std::vector<int32_t> parentIndices;
		std::vector<GfMatrix4d> bindXforms;
		std::vector<uint32_t> windSimulationGroupIndices;
		std::string windProfileIdentity;
		DynamicWindMetadata dynamicWindMetadata;
		double metersPerUnit = 1.0;
	};

	struct AssemblySkeletonTopologyHints
	{
		bool scanned = false;
		std::size_t naniteBindJointPairCount = 0u;
		std::unordered_set<std::string> naniteBindEdgeKeys;
	};

	struct LoadingCaches {
		std::unordered_map<std::string, MaterialTemplateRecord> materialTemplateCache;
        std::unordered_map<std::string, std::shared_ptr<Material>> resolvedMaterialCache;
		std::unordered_map<std::string, std::vector<std::shared_ptr<Mesh>>> meshCache;
		std::unordered_map<std::string, PreprocessedMeshRecord> preprocessedMeshCache;
		std::unordered_map<std::string, std::string> skippedPreprocessedMeshReasons;
		std::vector<std::shared_ptr<Mesh>> stageAssemblyMeshes;
		std::unordered_map<std::string, std::shared_ptr<TextureAsset>> textureCache;
		std::unordered_set<std::string> unresolvedTextureCache;
		std::vector<std::string> textureSearchRoots;
		std::function<std::optional<std::string>(std::string_view)> resolveResourcePath;
		//std::unordered_map<std::string, std::shared_ptr<UsdSkelSkeleton>> unprocessedSkeletons;
		std::unordered_map<std::string, UsdPrim> primsWithSkeletons;
		std::unordered_map<std::string, std::shared_ptr<Skeleton>> skeletonMap;
		std::unordered_map<const Skeleton*, PayloadSkeletonBuildMetadata> payloadSkeletonMetadata;
		std::unordered_map<std::string, AssemblySkeletonTopologyHints> assemblySkeletonTopologyHintsByStage;
		std::unordered_map<std::string, std::shared_ptr<Animation>> animationMap;
		// For storing nodes in the USD shader graph
		std::unordered_map<std::string, flecs::entity> nodeMap;

		void Clear() {
			materialTemplateCache.clear();
			resolvedMaterialCache.clear();
			meshCache.clear();
			preprocessedMeshCache.clear();
			skippedPreprocessedMeshReasons.clear();
			stageAssemblyMeshes.clear();
			textureCache.clear();
			unresolvedTextureCache.clear();
			textureSearchRoots.clear();
			resolveResourcePath = {};
			primsWithSkeletons.clear();
			skeletonMap.clear();
			payloadSkeletonMetadata.clear();
			assemblySkeletonTopologyHintsByStage.clear();
			animationMap.clear();
			nodeMap.clear();
		}
	};

    extern thread_local LoadingCaches loadingCache;
}
