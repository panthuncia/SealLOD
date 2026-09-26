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

#include <flecs.h>

#include "BasicRenderer/Assets/Material.h"
#include "Utilities/Utilities.h"
#include <BasicRenderer/Assets/MaterialFlags.h>
#include <BasicRenderer/Pipeline/PSOFlags.h>
#include "BasicRenderer/Assets/Texture.h"
#include "Resources/Sampler.h"
#include "BasicRenderer/Assets/Import/Filetypes.h"
#include "BasicRenderer/Scene/Scene.h"
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include <BasicRenderer/Assets/Import/ClusterLODUtilities.h>
#include "BasicRenderer/Scene/Animation/Skeleton.h"
#include "Assets/Cache/Skeleton/SkeletonArtifactCache.h"
#include "BasicRenderer/Scene/Components.h"
#include "BasicRenderer/Scene/Animation/AnimationController.h"
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

	static uint32_t GetUsdPointInstancerMaxInstances() {
		static std::function<uint32_t(void)> getMaxInstances;
		if (!getMaxInstances) {
			try {
				getMaxInstances = SettingsManager::GetInstance().getSettingGetter<uint32_t>("usdPointInstancerMaxInstances");
			}
			catch (...) {
				return 0u;
			}
		}

		try {
			return getMaxInstances();
		}
		catch (...) {
			return 0u;
		}
	}

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

	static void ApplyPointInstancerPScaleFallback(
		const UsdGeomPointInstancer& pointInstancer,
		const UsdTimeCode& timeCode,
		const std::vector<bool>& mask,
		VtArray<GfMatrix4d>* instanceTransforms)
	{
		if (instanceTransforms == nullptr || instanceTransforms->empty()) {
			return;
		}

		VtVec3fArray nativeScales;
		if (pointInstancer.GetScalesAttr().Get(&nativeScales, timeCode) && !nativeScales.empty()) {
			return;
		}

		UsdGeomPrimvarsAPI primvarsAPI(pointInstancer.GetPrim());
		UsdGeomPrimvar pscalePrimvar = primvarsAPI.FindPrimvarWithInheritance(TfToken("pscale"));
		if (!pscalePrimvar) {
			return;
		}

		VtFloatArray pscaleValues;
		if (!pscalePrimvar.ComputeFlattened(&pscaleValues, timeCode) || pscaleValues.empty()) {
			spdlog::warn(
				"PointInstancer '{}' authored primvars:pscale but it could not be flattened at geometry sample time {}; ignoring fallback scaling.",
				pointInstancer.GetPrim().GetPath().GetString(),
				timeCode.IsDefault() ? -1.0 : timeCode.GetValue());
			return;
		}

		std::vector<float> resolvedPscale;
		resolvedPscale.reserve(instanceTransforms->size());
		if (pscaleValues.size() == 1) {
			resolvedPscale.assign(instanceTransforms->size(), pscaleValues[0]);
		}
		else if (!mask.empty() && pscaleValues.size() == mask.size()) {
			for (size_t valueIndex = 0; valueIndex < mask.size(); ++valueIndex) {
				if (mask[valueIndex]) {
					resolvedPscale.push_back(pscaleValues[valueIndex]);
				}
			}
		}
		else if (pscaleValues.size() == instanceTransforms->size()) {
			resolvedPscale.assign(pscaleValues.begin(), pscaleValues.end());
		}
		else {
			spdlog::warn(
				"PointInstancer '{}' primvars:pscale count {} does not match masked instance count {}; ignoring fallback scaling.",
				pointInstancer.GetPrim().GetPath().GetString(),
				pscaleValues.size(),
				instanceTransforms->size());
			return;
		}

		if (resolvedPscale.size() != instanceTransforms->size()) {
			spdlog::warn(
				"PointInstancer '{}' resolved primvars:pscale count {} does not match instance transform count {}; ignoring fallback scaling.",
				pointInstancer.GetPrim().GetPath().GetString(),
				resolvedPscale.size(),
				instanceTransforms->size());
			return;
		}

		for (size_t instanceIndex = 0; instanceIndex < instanceTransforms->size(); ++instanceIndex) {
			GfMatrix4d scaleMatrix(1.0);
			scaleMatrix.SetScale(GfVec3d(resolvedPscale[instanceIndex]));
			(*instanceTransforms)[instanceIndex] = scaleMatrix * (*instanceTransforms)[instanceIndex];
		}
	}

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

	void ProcessMeshAndAnimations(
		const UsdPrim& prim,
		std::vector<std::shared_ptr<Mesh>>& meshes,
		UsdSkelCache& skelCache,
		const UsdStageRefPtr& stage,
		std::shared_ptr<Scene>& scene,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		bool isUSDZ) {

		UsdGeomMesh mesh(prim);
		if (!mesh) {
			return; // Not a mesh prim
		}

		if (IsBrNiflyCollisionMesh(mesh)) {
			spdlog::info("Skipping BRNifly collision mesh '{}'.", mesh.GetPrim().GetPath().GetString());
			return;
		}
		if (IsBrNiflyLODRenderMesh(mesh)) {
			spdlog::debug("Skipping BRNifly LOD mesh '{}'.", mesh.GetPrim().GetPath().GetString());
			return;
		}

		if (IsUnsupportedBrNiflySkinnedMesh(mesh)) {
			spdlog::info(
				"Skipping BRNifly skinned mesh '{}' until NIF skeleton pose updates are supported.",
				mesh.GetPrim().GetPath().GetString());
			return;
		}

		auto skinningQuery = USDGeometryExtractor::GetSkinningQuery(mesh, skelCache);

		UsdSkelBindingAPI bindingAPI(prim);
		std::shared_ptr<Skeleton> skeleton;
		VtTokenArray skelJointOrderRaw;
		VtTokenArray skelJointOrderMapped;

		if (bindingAPI) {
			UsdSkelSkeleton skel;
			if (bindingAPI.GetSkeleton(&skel)) {
				spdlog::info("Found skeleton on prim: {}", prim.GetName().GetString());
				if (const UsdSkelRoot skelRoot = UsdSkelRoot::Find(skel.GetPrim()))
					skelCache.Populate(skelRoot, UsdPrimDefaultPredicate);
				auto skelQuery = skelCache.GetSkelQuery(skel);

				skelJointOrderRaw = skelQuery.GetJointOrder();

				if (!skinningQuery) {
					throw std::runtime_error(
						"Mesh is skinned but no skinning query found.");
				}
				auto& mapper = skinningQuery->GetJointMapper();
				if (mapper && !mapper->IsIdentity()) {
					// Map the joint order to the skinning query
					mapper->Remap(skelJointOrderRaw, &skelJointOrderMapped);
				}
				else {
					skelJointOrderMapped = skelJointOrderRaw;
				}

				spdlog::info("Original skeleton joint order:");
				for (const auto& joint : skelJointOrderRaw) {
					spdlog::info("  {}", joint.GetString());
				}
				spdlog::info("Mapped skeleton joint order:");
				for (const auto& joint : skelJointOrderMapped) {
					spdlog::info("  {}", joint.GetString());
				}

				skeleton = ProcessSkeleton(skel, skelJointOrderRaw, skelQuery, scene, metersPerUnit);

				UsdSkelBindingAPI skelAPI(skel.GetPrim());
				UsdPrim animPrim;
				if (skelAPI.GetAnimationSource(&animPrim)) {
					spdlog::info("Found animation source for skeleton: {}", animPrim.GetPath().GetString());
					UsdSkelAnimation anim(animPrim);
					auto animQuery = skelCache.GetAnimQuery(anim);

					if (animQuery) {
						auto animation = ProcessAnimQuery(animQuery, stage, metersPerUnit, skelJointOrderRaw);
						skeleton->AddAnimation(animation);
						// TODO: Should sleletons be applied to all child entities? Or just to this one?
					}
				}
			}
		}

		std::vector<std::shared_ptr<Mesh>> processedMesh = ProcessMesh(mesh, stage, metersPerUnit, upRot, directory, isUSDZ, skelCache, skelJointOrderRaw, skelJointOrderMapped);
		// Push back all meshes
		for (auto& m : processedMesh) {
			meshes.push_back(m);
		}

		if (skeleton) {
			for (auto& skelMesh : meshes) {
				skelMesh->SetBaseSkin(skeleton);
			}
		}

	}

	void ProcessPointInstancer(
		const UsdGeomPointInstancer& pointInstancer,
		flecs::entity instancerEntity,
		std::unordered_set<std::string>& prototypeRootsToSkip,
		const UsdStageRefPtr& stage,
		std::shared_ptr<Scene>& scene,
		UsdSkelCache& skelCache,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		bool isUSDZ)
	{
		SdfPathVector prototypeTargets;
		if (!pointInstancer.GetPrototypesRel().GetTargets(&prototypeTargets)) {
			spdlog::warn("PointInstancer '{}' has no valid prototypes relationship targets.", pointInstancer.GetPrim().GetPath().GetString());
			return;
		}

		for (const auto& prototypeTarget : prototypeTargets) {
			prototypeRootsToSkip.insert(prototypeTarget.GetString());
		}

		const UsdTimeCode timeCode = GetUsdGeometrySampleTime(stage);

		VtIntArray protoIndices;
		if (!pointInstancer.GetProtoIndicesAttr().Get(&protoIndices, timeCode)) {
			spdlog::warn(
				"PointInstancer '{}' has no readable protoIndices at geometry sample time {}.",
				pointInstancer.GetPrim().GetPath().GetString(),
				timeCode.IsDefault() ? -1.0 : timeCode.GetValue());
			return;
		}

		std::vector<bool> mask = pointInstancer.ComputeMaskAtTime(timeCode);
		if (!mask.empty() && !UsdGeomPointInstancer::ApplyMaskToArray(mask, &protoIndices)) {
			spdlog::warn("PointInstancer '{}' mask application to protoIndices failed.", pointInstancer.GetPrim().GetPath().GetString());
			return;
		}

		VtArray<GfMatrix4d> instanceTransforms;
		if (!pointInstancer.ComputeInstanceTransformsAtTime(
			&instanceTransforms,
			timeCode,
			timeCode,
			UsdGeomPointInstancer::IncludeProtoXform,
			UsdGeomPointInstancer::ApplyMask)) {
			spdlog::warn("PointInstancer '{}' failed to compute instance transforms.", pointInstancer.GetPrim().GetPath().GetString());
			return;
		}

		ApplyPointInstancerPScaleFallback(pointInstancer, timeCode, mask, &instanceTransforms);

		const size_t emittedCount = std::min(instanceTransforms.size(), protoIndices.size());
		if (instanceTransforms.size() != protoIndices.size()) {
			spdlog::warn(
				"PointInstancer '{}' transform/proto index count mismatch (transforms={}, indices={}), clamping to {}.",
				pointInstancer.GetPrim().GetPath().GetString(),
				instanceTransforms.size(),
				protoIndices.size(),
				emittedCount);
		}

		const uint32_t maxInstances = GetUsdPointInstancerMaxInstances();
		if (maxInstances > 0u && emittedCount > static_cast<size_t>(maxInstances)) {
			spdlog::warn(
				"Skipping PointInstancer '{}' because it would emit {} instances (limit {}).",
				pointInstancer.GetPrim().GetPath().GetString(),
				emittedCount,
				maxInstances);
			return;
		}

		UsdGeomXformCache xformCache(timeCode);
		std::vector<std::vector<PointInstancerPrototypeRenderable>> renderablesByPrototype;
		renderablesByPrototype.resize(prototypeTargets.size());

		for (size_t prototypeIndex = 0; prototypeIndex < prototypeTargets.size(); ++prototypeIndex) {
			const auto& prototypeTarget = prototypeTargets[prototypeIndex];
			UsdPrim prototypeRoot = stage->GetPrimAtPath(prototypeTarget);
			if (!prototypeRoot) {
				spdlog::warn("PointInstancer '{}' references invalid prototype target '{}'.",
					pointInstancer.GetPrim().GetPath().GetString(),
					prototypeTarget.GetString());
				continue;
			}

			const GfMatrix4d prototypeRootWorldInverse = xformCache.GetLocalToWorldTransform(prototypeRoot).GetInverse();
			std::function<void(const UsdPrim&)> gatherPrototypeRenderables = [&](const UsdPrim& prototypePrim) {
				if (prototypePrim.IsA<UsdGeomImageable>()) {
					UsdGeomImageable imageable(prototypePrim);
					if (imageable.ComputeVisibility(timeCode) == UsdGeomTokens->invisible) {
						return;
					}
				}

				std::vector<std::shared_ptr<Mesh>> prototypePrimMeshes;
				ProcessMeshAndAnimations(prototypePrim, prototypePrimMeshes, skelCache, stage, scene, metersPerUnit, upRot, directory, isUSDZ);
				if (!prototypePrimMeshes.empty()) {
					PointInstancerPrototypeRenderable renderable;
					renderable.meshes = std::move(prototypePrimMeshes);
					renderable.localTransform = xformCache.GetLocalToWorldTransform(prototypePrim) * prototypeRootWorldInverse;
					renderable.name = prototypePrim.GetName().GetString();
					renderablesByPrototype[prototypeIndex].push_back(std::move(renderable));
				}

				for (const auto& childPrim : prototypePrim.GetFilteredChildren(UsdTraverseInstanceProxies())) {
					gatherPrototypeRenderables(childPrim);
				}
			};

			gatherPrototypeRenderables(prototypeRoot);

			if (renderablesByPrototype[prototypeIndex].empty()) {
				spdlog::warn("PointInstancer '{}' prototype '{}' resolved no renderable meshes.",
					pointInstancer.GetPrim().GetPath().GetString(),
					prototypeTarget.GetString());
			}
		}

		const std::string baseName = pointInstancer.GetPrim().GetName().GetString();

		for (size_t instanceIndex = 0; instanceIndex < emittedCount; ++instanceIndex) {
			const int prototypeIndex = protoIndices[instanceIndex];
			if (prototypeIndex < 0 || static_cast<size_t>(prototypeIndex) >= renderablesByPrototype.size()) {
				spdlog::warn("PointInstancer '{}' has out-of-range proto index {} at instance {}.",
					pointInstancer.GetPrim().GetPath().GetString(),
					prototypeIndex,
					instanceIndex);
				continue;
			}

			auto& prototypeRenderables = renderablesByPrototype[prototypeIndex];
			if (prototypeRenderables.empty()) {
				continue;
			}

			auto instanceEntity = scene->CreateNodeECS(s2ws(baseName + "_instance_" + std::to_string(instanceIndex)));
			SetEntityTransformFromUsdMatrix(instanceEntity, instanceTransforms[instanceIndex], metersPerUnit);
			instanceEntity.child_of(instancerEntity);

			for (const auto& prototypeRenderable : prototypeRenderables) {
				auto renderableEntity = scene->CreateRenderableEntityECS(
					prototypeRenderable.meshes,
					s2ws(prototypeRenderable.name.empty() ? baseName : prototypeRenderable.name));
				SetEntityTransformFromUsdMatrix(renderableEntity, prototypeRenderable.localTransform, metersPerUnit);
				renderableEntity.child_of(instanceEntity);
			}
		}
	}

	void ParseNodeHierarchy(std::shared_ptr<Scene> scene,
		const pxr::UsdStageRefPtr& stage,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		UsdSkelCache& skelCache,
		bool isUSDZ) {
		if (!loadingCache.stageAssemblyMeshes.empty()) {
			auto entity = scene->CreateRenderableEntityECS(
				loadingCache.stageAssemblyMeshes,
				L"__CLodAssembly");
			if (!entity.is_valid()) {
				spdlog::warn("USD point-instancer CLod assembly mesh was loaded, but synthetic entity creation failed.");
			}
			else {
				spdlog::info(
					"USD point-instancer CLod assembly renderable created with {} mesh(es); skipping expanded stage hierarchy.",
					loadingCache.stageAssemblyMeshes.size());
			}
			return;
		}

		std::unordered_set<std::string> prototypeRootsToSkip;
        const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);

		std::function<void(const UsdPrim& prim,
			flecs::entity parent, bool hasCorrectedAxis)> RecurseHierarchy = [&](const UsdPrim& prim, flecs::entity parent, bool hasCorrectedAxis) {
				if (prototypeRootsToSkip.contains(prim.GetPath().GetString())) {
					spdlog::info("Skipping PointInstancer prototype subtree root '{}' during normal traversal.", prim.GetPath().GetString());
					return;
				}

                if (prim.IsA<UsdGeomImageable>()) {
                    UsdGeomImageable imageable(prim);
                    if (imageable.ComputeVisibility(geomTimeCode) == UsdGeomTokens->invisible) {
                        spdlog::info("Skipping invisible prim subtree '{}'.", prim.GetPath().GetString());
                        return;
                    }
                }

				spdlog::info("Prim: {}", prim.GetName().GetString());

				GfVec3d translation = { 0, 0, 0 };
				GfQuaternion rot = GfQuaternion(1);
				GfVec3d scale = { 1, 1, 1 };
                bool resetsXformStack = false;
				// If this node has a transform, get it
				if (prim.IsA<UsdGeomXformable>()) {
					UsdGeomXformable xform(prim);
					GfMatrix4d mat;
                    xform.GetLocalTransformation(&mat, &resetsXformStack, geomTimeCode);

					// Serialize mat
					std::string matStr;
					for (int i = 0; i < 4; ++i) {
						for (int j = 0; j < 4; ++j) {
							matStr += std::to_string(mat[i][j]) + " ";
						}
						matStr += "\n";
					}

					spdlog::info("Xformable has transform: {}", matStr);

					if (!hasCorrectedAxis || resetsXformStack) { // Apply axis correction on detached transform roots too
						GfMatrix4d rotMat(upRot, GfVec3d(0.0));
						mat = mat * rotMat;
						hasCorrectedAxis = true;
					}

					// Decompose via GfTransform:
					GfTransform xf(mat);
					translation = xf.GetTranslation();
					rot = xf.GetRotation().GetQuaternion();   // as a quaternion
					scale = xf.GetScale();
				}



				std::vector<UsdPrim> childrenToRecurse;
				for (auto child : prim.GetFilteredChildren(UsdTraverseInstanceProxies())) {
					childrenToRecurse.push_back(child);
				}

				std::vector<std::shared_ptr<Mesh>> meshes;
				const bool isPointInstancer = prim.IsA<UsdGeomPointInstancer>();
				if (!isPointInstancer) {
					ProcessMeshAndAnimations(prim, meshes, skelCache, stage, scene, metersPerUnit, upRot, directory, isUSDZ);
				}

				flecs::entity entity;
				if (meshes.size() > 0) {
					entity = scene->CreateRenderableEntityECS(meshes, s2ws(prim.GetName().GetString()));
				}
				else {
					entity = scene->CreateNodeECS(s2ws(prim.GetName().GetString()));
				}
				loadingCache.nodeMap[prim.GetPath().GetString()] = entity;

				entity.set<Components::Position>({ DirectX::XMFLOAT3(static_cast<float>(translation[0] * metersPerUnit), static_cast<float>(translation[1] * metersPerUnit), static_cast<float>(translation[2] * metersPerUnit)) });
				entity.set<Components::Rotation>({ DirectX::XMFLOAT4(static_cast<float>(rot.GetImaginary()[0]), static_cast<float>(rot.GetImaginary()[1]), static_cast<float>(rot.GetImaginary()[2]), static_cast<float>(rot.GetReal())) });
				entity.set<Components::Scale>({ DirectX::XMFLOAT3(static_cast<float>(scale[0]), static_cast<float>(scale[1]), static_cast<float>(scale[2])) });

				if (parent && !resetsXformStack) {
					entity.child_of(parent);
				}
				else if (!prim.IsPseudoRoot() && !resetsXformStack) {
					spdlog::warn("Node {} has no parent", entity.name().c_str());
				}

				if (isPointInstancer) {
					ProcessPointInstancer(
						UsdGeomPointInstancer(prim),
						entity,
						prototypeRootsToSkip,
						stage,
						scene,
						skelCache,
						metersPerUnit,
						upRot,
						directory,
						isUSDZ);
				}

				for (auto& child : childrenToRecurse) {
					if (prototypeRootsToSkip.contains(child.GetPath().GetString())) {
						spdlog::info("Skipping PointInstancer prototype child '{}' during normal traversal.", child.GetPath().GetString());
						continue;
					}
					RecurseHierarchy(child, entity, hasCorrectedAxis);
				}
			};

		RecurseHierarchy(stage->GetPseudoRoot(), flecs::entity(), false);

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

	std::shared_ptr<Scene> LoadModelFromStage(
		const UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings) {
		if (!stage) {
			spdlog::error("USD stage open failed for in-memory source '{}'", options.sourceIdentifier);
			return nullptr;
		}

		// Grab the context USD created for this stage:
		auto ctx = stage->GetPathResolverContext();

		// Bind it (in this thread) so Resolve() knows about local files:
		ArResolverContextBinder binder(ctx);

		spdlog::info("Context empty? {}", ctx.IsEmpty());
		spdlog::info("Context debug string: {}", ArGetDebugString(ctx));

		if (auto defCtx = ctx.Get<ArDefaultResolverContext>()) {
			for (auto& p : defCtx->GetSearchPath()) {
				spdlog::info("  search path: {}", p);
			}
		}

		for (auto& layer : stage->GetLayerStack()) {
			spdlog::info("Loaded layer: {}", layer->GetIdentifier());
		}

		const auto stageContext = MakeStageImportContext(stage, options);
		loadingCache.textureSearchRoots = options.textureSearchRoots;
		loadingCache.resolveResourcePath = importSettings.resolveResourcePath;
		if (!stageContext.directory.empty() &&
			std::find(loadingCache.textureSearchRoots.begin(), loadingCache.textureSearchRoots.end(), stageContext.directory) == loadingCache.textureSearchRoots.end()) {
			loadingCache.textureSearchRoots.push_back(stageContext.directory);
		}

		const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);
		UsdSkelCache skelCache;
		std::string assetAssemblyFallbackReason;
		auto assetAssemblyBuckets = DiscoverAssetAssemblyBuckets(
			stage,
			skelCache,
			stageContext.metersPerUnit,
			geomTimeCode,
			options,
			importSettings,
			&assetAssemblyFallbackReason);

		std::vector<std::shared_ptr<Mesh>> assetAssemblyMeshes;
		if (!assetAssemblyBuckets.empty() &&
			TryLoadAssetAssemblyMeshes(
				stage,
				stageContext,
				importSettings,
				options,
				options.sourceIdentifier,
				geomTimeCode,
				assetAssemblyBuckets,
				assetAssemblyMeshes)) {
			spdlog::info(
				"USD whole-asset CLod assembly cache hit: buckets={}, renderables={}.",
				assetAssemblyBuckets.size(),
				assetAssemblyMeshes.size());
			auto scene = CreateCollapsedAssetAssemblyScene(
				assetAssemblyMeshes,
				assetAssemblyBuckets,
				stageContext.upRot);
			loadingCache.Clear();
			return scene;
		}

		if (!assetAssemblyBuckets.empty()) {
			spdlog::debug(
				"USD whole-asset CLod assembly cache miss for {} bucket(s); falling back to expanded CPU-side instancing.",
				assetAssemblyBuckets.size());
		}

		{
			ZoneScopedN("USDLoader::LoadModelFromStage::BuildOrFallback");
			std::string assemblyBuildFailureReason;
			if (!assetAssemblyFallbackReason.empty()) {
				spdlog::debug("USD whole-asset CLod assembly fallback: {}", assetAssemblyFallbackReason);
			}
			else if (!assetAssemblyBuckets.empty()) {
				spdlog::debug("USD whole-asset CLod assembly fallback: full assembly cache was missing.");
			}
			else {
				spdlog::debug("USD whole-asset CLod assembly fallback: cache/build path did not produce renderables.");
			}
			PreprocessAllMeshes(
				stage,
				stageContext.metersPerUnit,
				stageContext.directory,
				stageContext.isUSDZ,
				importSettings,
				options,
				options.sourceIdentifier,
				!assetAssemblyBuckets.empty());

			if (!assetAssemblyBuckets.empty() &&
				BuildAssetAssemblyMeshesFromPreprocessedData(
					stage,
					stageContext,
					importSettings,
					options,
					options.sourceIdentifier,
					geomTimeCode,
					assetAssemblyBuckets,
					assetAssemblyMeshes,
					&assemblyBuildFailureReason)) {
				spdlog::info(
					"USD whole-asset CLod assembly built after cache miss: buckets={}, renderables={}.",
					assetAssemblyBuckets.size(),
					assetAssemblyMeshes.size());
				auto scene = CreateCollapsedAssetAssemblyScene(
					assetAssemblyMeshes,
					assetAssemblyBuckets,
					stageContext.upRot);
				loadingCache.Clear();
				return scene;
			}

			spdlog::warn(
				"USD whole-asset CLod assembly build failed; using expanded hierarchy fallback: {}.",
				assemblyBuildFailureReason.empty() ? "builder produced no renderables" : assemblyBuildFailureReason);
			auto scene = std::make_shared<Scene>();
			ParseNodeHierarchy(scene, stage, stageContext.metersPerUnit, stageContext.upRot, stageContext.directory, skelCache, stageContext.isUSDZ);
			loadingCache.Clear();
			return scene;
		}
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
