#include <spdlog/spdlog.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <chrono>
#include <queue>
#include <set>
#include <limits>

#include <nlohmann/json.hpp>
#include <tracy/Tracy.hpp>

#include <boost/functional/hash.hpp>

#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/defaultResolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/ar/packageUtils.h>
//#include <pxr/usd/ar/packageResolver.h>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/primFlags.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/utils.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/primvar.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdSkel/skeleton.h>
#include <pxr/usd/usdSkel/animation.h>
#include <pxr/usd/usdSkel/bindingAPI.h>
#include <pxr/usd/usdSkel/skeletonQuery.h>
#include <pxr/usd/usdSkel/cache.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/transform.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/plug/registry.h>


#include "BasicRenderer/Assets/Material.h"
#include "Utilities/Utilities.h"
#include <BasicRenderer/Assets/MaterialFlags.h>
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include "BasicRenderer/Assets/Texture.h"
#include "Resources/Sampler.h"
#include "BasicRenderer/Assets/Import/Filetypes.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include "Assets/Cache/Skeleton/SkeletonArtifactCache.h"
#include "BasicRenderer/Scene/Components.h"
#include "Runtime/Settings/SettingsManager.h"
#include <BasicRenderer/Streaming/TaskScheduler.h>
#include "Assets/Textures/Processing/TextureProcessingManager.h"

#include "BasicRenderer/Assets/USD/USDLoader.h"
#include "Assets/Import/USD/USDImportState.h"
#include "Assets/Import/USD/USDMaterialConversion.h"
#include "Assets/Import/USD/USDSkeletonConversion.h"
#include "Assets/Import/USD/USDGeometryConversion.h"
#include "Assets/Import/USD/USDStageHelpers.h"
#include "Assets/Import/USD/USDAssetAssembly.h"
#include <BasicRenderer/Assets/Import/USDMaterialCache.h>
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.h"
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

namespace USDLoader {

	std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin)
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - begin).count());
	}

	using namespace pxr;
	using json = nlohmann::json;

	thread_local LoadingCaches loadingCache;

	static GfRotation GetStageUpAxisCorrection(const UsdStageRefPtr& stage) {
		TfToken upAxis = UsdGeomGetStageUpAxis(stage);
		if (upAxis == UsdGeomTokens->z) {
			return GfRotation(GfVec3d(1, 0, 0), -90.0);
		}
		if (upAxis == UsdGeomTokens->y) {
			return GfRotation(GfVec3d(0, 1, 0), 0);
		}
		if (upAxis == UsdGeomTokens->x) {
			return GfRotation(GfVec3d(0, 1, 0), -90.0);
		}

		spdlog::warn("Unknown Up Axis: {}", upAxis.GetString());
		return {};
	}

	static StageImportContext MakeStageImportContext(
		const UsdStageRefPtr& stage,
		const InMemoryStageOptions& options) {
		StageImportContext context;
		context.metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
		context.upRot = GetStageUpAxisCorrection(stage);
		context.directory = options.sourceDirectory;
		context.isUSDZ = options.isUsdPackage;
		return context;
	}

	struct PointInstancerPrototypeRenderable {
		std::vector<std::shared_ptr<Mesh>> meshes;
		GfMatrix4d localTransform = GfMatrix4d(1.0);
		std::string name;
	};

	struct PayloadMeshResult
	{
		std::vector<std::shared_ptr<Mesh>> meshes;
		std::vector<br::import::RenderablePrototypeGeometry> prototypeGeometries;
	};

	PayloadMeshResult ProcessMeshForPayload(
		const UsdPrim& prim,
		UsdSkelCache& skelCache,
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		bool isUSDZ)
	{
		ZoneScopedN("USDLoader::ProcessMeshForPayload");
		const auto primPath = prim.GetPath().GetString();
		ZoneText(primPath.data(), primPath.size());
		UsdGeomMesh mesh(prim);
		if (!mesh || IsUnsupportedBrNiflySkinnedMesh(mesh)) {
			return {};
		}
		if (IsBrNiflyCollisionMesh(mesh)) {
			return {};
		}
		if (IsBrNiflyLODRenderMesh(mesh)) {
			spdlog::debug("Skipping BRNifly LOD mesh '{}'.", mesh.GetPrim().GetPath().GetString());
			return {};
		}

		std::optional<UsdSkelSkinningQuery> skinningQuery;
		{
			ZoneScopedN("USDLoader::ProcessMeshForPayload::GetSkinningQuery");
			skinningQuery = USDGeometryExtractor::GetSkinningQuery(mesh, skelCache);
		}
		UsdSkelBindingAPI bindingAPI(prim);
		std::shared_ptr<Skeleton> skeleton;
		VtTokenArray skelJointOrderRaw;
		VtTokenArray skelJointOrderMapped;

		if (bindingAPI) {
			UsdSkelSkeleton skel;
			if (bindingAPI.GetSkeleton(&skel)) {
				if (const UsdSkelRoot skelRoot = UsdSkelRoot::Find(skel.GetPrim()))
					skelCache.Populate(skelRoot, UsdPrimDefaultPredicate);
				auto skelQuery = skelCache.GetSkelQuery(skel);
				skelJointOrderRaw = skelQuery.GetJointOrder();

				if (!skinningQuery) {
					throw std::runtime_error("Mesh is skinned but no skinning query found.");
				}

				auto& mapper = skinningQuery->GetJointMapper();
				if (mapper && !mapper->IsIdentity()) {
					mapper->Remap(skelJointOrderRaw, &skelJointOrderMapped);
				}
				else {
					skelJointOrderMapped = skelJointOrderRaw;
				}

				skeleton = BuildPayloadSkeleton(skel, skelJointOrderRaw, skelQuery, metersPerUnit);
				AddPayloadSkeletonAnimation(skel, skelCache, stage, metersPerUnit, skelJointOrderRaw, skeleton);
			}
		}

		auto processedMeshes = ProcessMesh(mesh, stage, metersPerUnit, upRot, directory, isUSDZ, skelCache, skelJointOrderRaw, skelJointOrderMapped);
		std::vector<br::import::RenderablePrototypeGeometry> prototypeGeometries;
		if (const auto preprocessedIt = loadingCache.preprocessedMeshCache.find(mesh.GetPrim().GetPath().GetString());
			preprocessedIt != loadingCache.preprocessedMeshCache.end()) {
			prototypeGeometries.reserve(preprocessedIt->second.subsets.size());
			for (const auto& subset : preprocessedIt->second.subsets) {
				prototypeGeometries.push_back(subset.result.prototypeGeometry);
			}
		}
		if (skeleton) {
			ZoneScopedN("USDLoader::ProcessMeshForPayload::AttachSkeleton");
			for (auto& processedMesh : processedMeshes) {
				if (processedMesh) {
					processedMesh->SetBaseSkin(skeleton);
				}
			}
		}
		return PayloadMeshResult{
			.meshes = std::move(processedMeshes),
			.prototypeGeometries = std::move(prototypeGeometries),
		};
	}

	ImportedAssetPayload ParseImportedAssetPayload(
		const pxr::UsdStageRefPtr& stage,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		UsdSkelCache& skelCache,
		bool isUSDZ)
	{
		ZoneScopedN("USDLoader::ParseImportedAssetPayload");
		ImportedAssetPayload payload;
		if (!loadingCache.stageAssemblyMeshes.empty()) {
			RenderablePartPayload part;
			// Assembly instance transforms are stored in the cache after conversion
			// from the stage's authored up-axis into renderer space.
			part.localMatrix = DirectX::XMMatrixIdentity();
			part.name = "__CLodAssembly";
			for (const auto& mesh : loadingCache.stageAssemblyMeshes) {
				if (!mesh) {
					continue;
				}
				part.meshes.push_back(mesh);
				payload.meshes.push_back(mesh);
			}
			if (!part.meshes.empty()) {
				payload.parts.push_back(std::move(part));
				spdlog::info(
					"USD payload import using point-instancer CLod assembly with {} mesh(es).",
					payload.meshes.size());
			}
			return payload;
		}
		std::unordered_set<std::uint64_t> meshIDs;
		std::uint32_t skinnedShapeIndex = 0;
		const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);

		std::function<void(const UsdPrim&, DirectX::XMMATRIX, bool)> recurse =
			[&](const UsdPrim& prim, DirectX::XMMATRIX parentMatrix, bool hasCorrectedAxis) {
				if (prim.IsA<UsdGeomImageable>()) {
					UsdGeomImageable imageable(prim);
					if (imageable.ComputeVisibility(geomTimeCode) == UsdGeomTokens->invisible) {
						return;
					}
				}

				GfMatrix4d localUsdMatrix(1.0);
				bool resetsXformStack = false;
				bool nextHasCorrectedAxis = hasCorrectedAxis;
				if (prim.IsA<UsdGeomXformable>()) {
					UsdGeomXformable xform(prim);
					xform.GetLocalTransformation(&localUsdMatrix, &resetsXformStack, geomTimeCode);
					if (IsBrNiflyObjectRootPrim(prim)) {
						// Skyrim places the loaded NIF root with the reference transform. Treat
						// the authored top object-root transform as replaceable placement state,
						// not reusable asset-local geometry offset.
						localUsdMatrix = GfMatrix4d(1.0);
					}
					if (!nextHasCorrectedAxis || resetsXformStack) {
						GfMatrix4d rotMat(upRot, GfVec3d(0.0));
						localUsdMatrix = localUsdMatrix * rotMat;
						nextHasCorrectedAxis = true;
					}
				}

				const auto localMatrix = DirectXMatrixFromUsdMatrix(localUsdMatrix, metersPerUnit);
				const auto worldMatrix = resetsXformStack ? localMatrix : localMatrix * parentMatrix;

				if (prim.IsA<UsdGeomPointInstancer>()) {
					spdlog::warn(
						"USD payload import currently skips PointInstancer '{}'; use the scene import path for instanced USD assets.",
						prim.GetPath().GetString());
					return;
				}

				auto meshResult = ProcessMeshForPayload(prim, skelCache, stage, metersPerUnit, upRot, directory, isUSDZ);
				if (!meshResult.meshes.empty()) {
					RenderablePartPayload part;
					part.localMatrix = worldMatrix;
					part.name = prim.GetName().GetString();
					part.prototypeGeometries = std::move(meshResult.prototypeGeometries);

					bool hasSkinnedMesh = false;
					for (const auto& mesh : meshResult.meshes) {
						if (!mesh) {
							continue;
						}
						part.meshes.push_back(mesh);
						hasSkinnedMesh = hasSkinnedMesh || ((mesh->GetPerMeshCBData().vertexFlags & VERTEX_SKINNED) != 0u);
						if (meshIDs.insert(mesh->GetGlobalID()).second) {
							payload.meshes.push_back(mesh);
						}
					}
					if (!part.meshes.empty()) {
						if (hasSkinnedMesh) {
							part.skinnedShapeIndex = skinnedShapeIndex++;
						}
						payload.parts.push_back(std::move(part));
					}
				}

				for (auto child : prim.GetFilteredChildren(UsdTraverseInstanceProxies())) {
					recurse(child, worldMatrix, nextHasCorrectedAxis);
				}
			};

		{
			ZoneScopedN("USDLoader::ParseImportedAssetPayload::TraverseStage");
			recurse(stage->GetPseudoRoot(), DirectX::XMMatrixIdentity(), false);
		}
		TracyPlot("SARP.Import.USD.Payload.Meshes", static_cast<int64_t>(payload.meshes.size()));
		TracyPlot("SARP.Import.USD.Payload.Parts", static_cast<int64_t>(payload.parts.size()));
		return payload;
	}

	std::optional<ImportedAssetPayload> LoadImportedAssetFromStage(
		const UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings,
		ImportTimingStats* timingStats) {
		ZoneScopedN("USDLoader::LoadImportedAssetFromStage");
		ZoneText(options.sourceIdentifier.data(), options.sourceIdentifier.size());
		if (!stage) {
			spdlog::error("USD payload stage open failed for in-memory source '{}'", options.sourceIdentifier);
			return std::nullopt;
		}

		auto ctx = stage->GetPathResolverContext();
		ArResolverContextBinder binder(ctx);

		const auto stageContext = MakeStageImportContext(stage, options);
		loadingCache.textureSearchRoots = options.textureSearchRoots;
		loadingCache.resolveResourcePath = importSettings.resolveResourcePath;
		if (!stageContext.directory.empty() &&
			std::find(loadingCache.textureSearchRoots.begin(), loadingCache.textureSearchRoots.end(), stageContext.directory) == loadingCache.textureSearchRoots.end()) {
			loadingCache.textureSearchRoots.push_back(stageContext.directory);
		}

		// Payload imports are the path used by SARP asset overrides.  Keep them on
		// the same whole-asset assembly path as scene imports so skinned assembly
		// caches retain their expanded skeleton and DynamicWind metadata.
		const UsdTimeCode assemblyTimeCode = GetUsdGeometrySampleTime(stage);
		UsdSkelCache assemblySkelCache;
		std::string assemblyFallbackReason;
		// BRNifly skin metadata is mesh-local rather than UsdSkel-authored. The
		// generic whole-asset assembly path deliberately discards that metadata,
		// so TREE candidates stay on the ordinary per-mesh CLod payload path where
		// it can be promoted into a BRSKEL artifact below.
		std::vector<AssetAssemblyBucketInfo> assemblyBuckets;
		const bool isGrassPrototypePayload =
			options.sourceIdentifier.find("#sarp-grass-prototype-v2") != std::string::npos;
		if (!isGrassPrototypePayload &&
			!importSettings.enableNifTreeProceduralWind &&
			!importSettings.prepareObjectReyesAtlasRecipes) {
			assemblyBuckets = DiscoverAssetAssemblyBuckets(
				stage,
				assemblySkelCache,
				stageContext.metersPerUnit,
				assemblyTimeCode,
				options,
				importSettings,
				&assemblyFallbackReason);
		}
		if (assemblyBuckets.empty() && options.requireWholeAssetAssembly) {
			spdlog::error(
				"Required USD whole-asset CLod assembly could not be discovered for '{}': {}.",
				options.sourceIdentifier,
				assemblyFallbackReason.empty() ? "no assembly-compatible material buckets" : assemblyFallbackReason);
			loadingCache.Clear();
			return std::nullopt;
		}
		if (!assemblyBuckets.empty()) {
			auto makeAssemblyPayload = [&](std::vector<std::shared_ptr<Mesh>> assemblyMeshes,
				std::string_view source) -> std::optional<ImportedAssetPayload> {
				ImportedAssetPayload payload;
				RenderablePartPayload part;
				part.localMatrix = DirectX::XMMatrixIdentity();
				part.name = "__CLodAssetAssembly";
				std::vector<USDMaterialCache::AssemblyMaterialEntry> manifest;
				manifest.reserve(assemblyMeshes.size());
				for (size_t i = 0; i < assemblyMeshes.size(); ++i) {
					auto& mesh = assemblyMeshes[i];
					if (!mesh) continue;
					payload.meshes.push_back(mesh);
					part.meshes.push_back(mesh);
					if (i < assemblyBuckets.size()) {
						if (auto identity = BuildAssetAssemblyIdentity(
							stage, options.sourceIdentifier, assemblyTimeCode, assemblyBuckets[i])) {
							const auto material = mesh->material ? mesh->material : Material::GetDefaultMaterial();
							manifest.push_back(USDMaterialCache::AssemblyMaterialEntry{
								.identity = std::move(*identity),
								.material = material->ToCacheDescription(),
							});
						}
					}
				}
				if (payload.meshes.empty()) {
					return std::nullopt;
				}
				if (manifest.size() != payload.meshes.size()) {
					spdlog::error(
						"USD payload whole-asset assembly could not derive identities for every material bucket: source='{}' identities={} renderables={}.",
						options.sourceIdentifier, manifest.size(), payload.meshes.size());
					return std::nullopt;
				}
				if (!USDMaterialCache::SaveAssemblyMaterialManifest(options.sourceIdentifier, manifest)) {
					spdlog::error(
						"USD payload whole-asset assembly manifest save failed for '{}'.",
						options.sourceIdentifier);
					return std::nullopt;
				}
				payload.parts.push_back(std::move(part));
				spdlog::info(
					"USD payload whole-asset CLod assembly {}: source='{}' buckets={} renderables={}.",
					source, options.sourceIdentifier, assemblyBuckets.size(), payload.meshes.size());
				loadingCache.Clear();
				return payload;
			};

			std::vector<std::shared_ptr<Mesh>> assemblyMeshes;
			if (TryLoadAssetAssemblyMeshes(
				stage,
				stageContext,
				importSettings,
				options,
				options.sourceIdentifier,
				assemblyTimeCode,
				assemblyBuckets,
				assemblyMeshes)) {
				return makeAssemblyPayload(std::move(assemblyMeshes), "loaded from cache");
			}

			PreprocessAllMeshes(
				stage,
				stageContext.metersPerUnit,
				stageContext.directory,
				stageContext.isUSDZ,
				importSettings,
				options,
				options.sourceIdentifier,
				true);
			std::string assemblyBuildFailureReason;
			if (BuildAssetAssemblyMeshesFromPreprocessedData(
				stage,
				stageContext,
				importSettings,
				options,
				options.sourceIdentifier,
				assemblyTimeCode,
				assemblyBuckets,
				assemblyMeshes,
				&assemblyBuildFailureReason)) {
				return makeAssemblyPayload(std::move(assemblyMeshes), "built");
			}
			spdlog::error(
				"USD payload whole-asset assembly build failed for '{}': {}",
				options.sourceIdentifier,
				assemblyBuildFailureReason.empty() ? "builder produced no renderables" : assemblyBuildFailureReason);
			if (options.requireWholeAssetAssembly) {
				loadingCache.Clear();
				return std::nullopt;
			}
		}

		try {
			UsdSkelCache skelCache;

			{
				const auto begin = std::chrono::steady_clock::now();
				const auto pointInstancerAssemblyIdentity = BuildPointInstancerAssemblyIdentity(
					stage, options.sourceIdentifier, GetUsdGeometrySampleTime(stage));
				if (pointInstancerAssemblyIdentity) {
					TryLoadPointInstancerAssemblyMesh(
						stage, GetUsdGeometrySampleTime(stage), options.sourceIdentifier);
				}
				if (loadingCache.stageAssemblyMeshes.empty()) {
					PreprocessAllMeshes(
						stage,
						stageContext.metersPerUnit,
						stageContext.directory,
						stageContext.isUSDZ,
						importSettings,
						options,
						options.sourceIdentifier);
				}
				if (timingStats) {
					timingStats->meshPreprocessMs += ElapsedMs(begin);
				}
			}
			auto payload = [&]() {
				const auto begin = std::chrono::steady_clock::now();
				auto result = ParseImportedAssetPayload(stage, stageContext.metersPerUnit, stageContext.upRot, stageContext.directory, skelCache, stageContext.isUSDZ);
				if (timingStats) {
					timingStats->payloadParseMs += ElapsedMs(begin);
				}
				return result;
			}();
			if (importSettings.enableNifTreeProceduralWind) {
				ZoneScopedN("USDLoader::LoadImportedAssetFromStage::AttachBrNiflyTreeWindSkeletons");
				AttachBrNiflyTreeWindSkeletons(stage, payload, options.sourceIdentifier);
			}
			{
				ZoneScopedN("USDLoader::LoadImportedAssetFromStage::ClearLoadingCache");
				loadingCache.Clear();
			}
			return payload;
		}
		catch (...) {
			ZoneScopedN("USDLoader::LoadImportedAssetFromStage::ClearLoadingCacheAfterException");
			loadingCache.Clear();
			throw;
		}
	}

	std::optional<ImportedAssetPayload> LoadImportedAssetFromFile(
		const std::string& filePath,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings,
		ImportTimingStats* timingStats) {
		ZoneScopedN("USDLoader::LoadImportedAssetFromFile");
		ZoneText(filePath.data(), filePath.size());

		std::string canonicalFileKey = std::filesystem::absolute(filePath).lexically_normal().generic_string();
		std::transform(canonicalFileKey.begin(), canonicalFileKey.end(), canonicalFileKey.begin(),
			[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
		static std::mutex fileLoadMutexTableGuard;
		static std::unordered_map<std::string, std::unique_ptr<std::mutex>> fileLoadMutexTable;
		std::mutex* fileLoadMutex = nullptr;
		{
			std::lock_guard tableLock(fileLoadMutexTableGuard);
			auto& entry = fileLoadMutexTable[canonicalFileKey];
			if (!entry) {
				entry = std::make_unique<std::mutex>();
			}
			fileLoadMutex = entry.get();
		}
		std::lock_guard fileLoadLock(*fileLoadMutex);

		auto tryLoadCachedMaterialAssembly = [&]() -> std::optional<ImportedAssetPayload> {
			auto manifest = USDMaterialCache::LoadAssemblyMaterialManifest(options.sourceIdentifier);
			if (!manifest || manifest->empty()) return std::nullopt;

			loadingCache.textureSearchRoots = options.textureSearchRoots;
			loadingCache.resolveResourcePath = importSettings.resolveResourcePath;
			if (!options.sourceDirectory.empty() &&
				std::find(loadingCache.textureSearchRoots.begin(), loadingCache.textureSearchRoots.end(), options.sourceDirectory) == loadingCache.textureSearchRoots.end()) {
				loadingCache.textureSearchRoots.push_back(options.sourceDirectory);
			}
			ImportedAssetPayload payload;
			RenderablePartPayload part;
			part.localMatrix = DirectX::XMMatrixIdentity();
			part.name = "__CachedMaterialCLodAssembly";
			for (auto& entry : *manifest) {
				auto prebuilt = CLodCacheLoader::TryLoadPrebuilt(entry.identity);
				if (!prebuilt || prebuilt->assemblyInstances.empty()) {
					loadingCache.Clear();
					spdlog::warn("USD cached material assembly is incomplete for source '{}' material '{}'.", options.sourceIdentifier, entry.material.name);
					return std::nullopt;
				}
				LoadSourcePathTextures(entry.material, {}, importSettings.loadMaterialTextures);
				auto material = Material::CreateShared(entry.material);
				const auto cachedJointCount = prebuilt->assemblySkeletonArtifact.jointCount;
				auto mesh = BuildMeshFromAssetAssemblyPrebuilt(std::move(prebuilt), material);
				if (!mesh) {
					loadingCache.Clear();
					return std::nullopt;
				}
				if (cachedJointCount != 0u) {
					spdlog::info(
						"USD cached material assembly skeleton restored: source='{}' material='{}' joints={} hasBaseSkin={} windEnabled={}.",
						options.sourceIdentifier, entry.material.name, cachedJointCount,
						mesh->HasBaseSkin(), mesh->HasBaseSkin() && mesh->GetBaseSkin()->HasWindSimulationGroups());
				}
				payload.meshes.push_back(mesh);
				part.meshes.push_back(std::move(mesh));
			}
			payload.parts.push_back(std::move(part));
			loadingCache.Clear();
			spdlog::info("USD cached material CLod assembly loaded without opening source: source='{}' buckets={}.", options.sourceIdentifier, payload.meshes.size());
			return payload;
		};
		if (auto cachedPayload = tryLoadCachedMaterialAssembly()) {
			return cachedPayload;
		}
		if (options.requireCachedAssembly) {
			spdlog::error("Required USD cached material CLod assembly is unavailable for '{}'; source USD will not be opened.", options.sourceIdentifier);
			return std::nullopt;
		}

		static std::mutex memoizedAssemblyGuard;
		static std::unordered_map<std::string, CLodCacheLoader::MeshCacheIdentity> memoizedAssemblyIdentities;
		auto tryLoadMemoizedAssembly = [&]() -> std::optional<ImportedAssetPayload> {
			std::optional<CLodCacheLoader::MeshCacheIdentity> identity;
			{
				std::lock_guard memoLock(memoizedAssemblyGuard);
				if (const auto it = memoizedAssemblyIdentities.find(canonicalFileKey);
					it != memoizedAssemblyIdentities.end()) {
					identity = it->second;
				}
			}
			if (!identity) {
				return std::nullopt;
			}
			auto prebuilt = CLodCacheLoader::TryLoadPrebuilt(*identity);
			if (!prebuilt || prebuilt->assemblyInstances.empty()) {
				std::lock_guard memoLock(memoizedAssemblyGuard);
				memoizedAssemblyIdentities.erase(canonicalFileKey);
				return std::nullopt;
			}
			MeshIngestBuilder ingest(0u, 0u, 0u, GetDefaultBuilderSettings());
			auto mesh = ingest.Build(
				Material::GetDefaultMaterial(), std::move(prebuilt), MeshCpuDataPolicy::ReleaseAfterUpload);
			if (!mesh) {
				return std::nullopt;
			}
			ImportedAssetPayload payload;
			payload.meshes.push_back(mesh);
			RenderablePartPayload part;
			part.meshes.push_back(std::move(mesh));
			part.localMatrix = DirectX::XMMatrixIdentity();
			part.name = "__CLodAssembly";
			payload.parts.push_back(std::move(part));
			spdlog::info("USD payload import reused memoized point-instancer CLod assembly for '{}'.", filePath);
			return payload;
		};
		if (auto memoizedPayload = tryLoadMemoizedAssembly()) {
			return memoizedPayload;
		}

		UsdStageRefPtr stage;
		{
			ZoneScopedN("USDLoader::LoadImportedAssetFromFile::UsdStageOpen");
			const auto begin = std::chrono::steady_clock::now();
			stage = UsdStage::Open(filePath);
			if (timingStats) {
				timingStats->stageOpenMs += ElapsedMs(begin);
			}
		}
		if (!stage) {
			spdlog::error("USD payload stage open failed for {}", filePath);
			return std::nullopt;
		}

		auto payload = LoadImportedAssetFromStage(stage, options, importSettings, timingStats);
		if (payload) {
			if (auto identity = BuildPointInstancerAssemblyIdentity(
				stage, options.sourceIdentifier, GetUsdGeometrySampleTime(stage));
				identity && CLodCacheLoader::TryLoadPrebuilt(*identity).has_value()) {
				std::lock_guard memoLock(memoizedAssemblyGuard);
				memoizedAssemblyIdentities.insert_or_assign(canonicalFileKey, std::move(*identity));
			}
		}
		return payload;
	}

}
