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
#include <BasicRenderer/Assets/Import/USDMaterialCache.h>
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.h"
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

#include "Assets/Import/USD/USDSkeletonConversion.h"

namespace USDLoader {
	using namespace pxr;
	using json = nlohmann::json;

	std::shared_ptr<Skeleton> BuildBrNiflyTreeWindSkeleton(
		const UsdStageRefPtr& stage,
		const Mesh& mesh,
		std::string_view sourceIdentifier)
	{
		const auto jointNamesSpan = mesh.GetSkinJointNames();
		const auto inverseBindsSpan = mesh.GetSkinInverseBindMatrices();
		if (!stage || jointNamesSpan.empty() || inverseBindsSpan.size() != jointNamesSpan.size()) {
			return nullptr;
		}

		const std::size_t count = jointNamesSpan.size();
		ClusterLODAssemblySkeletonData source;
		source.jointNames.assign(jointNamesSpan.begin(), jointNamesSpan.end());
		source.parentIndices.assign(count, -1);
		source.inverseBindMatrices.resize(count);
		source.restLocalMatrices.resize(count);
		source.bindGlobalMatrices.resize(count);
		source.windSimulationGroupIndices.resize(count, 1u);
		// The identity also provides a stable phase namespace. Distinct skin
		// palettes belonging to this NIF therefore evaluate matching named bones
		// with the same harmonic phase.
		source.windProfileIdentity = std::string(sourceIdentifier);
		source.dynamicWindMetadata.enabled = true;
		source.dynamicWindMetadata.maximumLodVariants = 16u;
		source.dynamicWindMetadata.maximumAdjacentBoneRatio = 1.75f;
		source.dynamicWindMetadata.groups.resize(2u);
		auto& trunkGroup = source.dynamicWindMetadata.groups[0];
		trunkGroup.flags = DynamicWindMetadata::GroupFlagTrunk;
		trunkGroup.role = DynamicWindSimulationGroupRole::Trunk;
		trunkGroup.profileGroupId = 0u;
		trunkGroup.reductionPriority = 4.0f;
		trunkGroup.minimumDriverCount = 2u;
		auto& branchGroup = source.dynamicWindMetadata.groups[1];
		branchGroup.role = DynamicWindSimulationGroupRole::DetailBranch;
		branchGroup.profileGroupId = 1u;
		branchGroup.reductionPriority = 2.0f;

		auto lower = [](std::string_view value) {
			std::string result(value);
			std::ranges::transform(result, result.begin(), [](unsigned char ch) {
				return static_cast<char>(std::tolower(ch));
			});
			return result;
		};
		std::unordered_map<std::string, UsdPrim> primByName;
		for (const auto& prim : stage->Traverse()) {
			if (prim.IsA<UsdGeomXformable>()) {
				primByName.try_emplace(lower(prim.GetName().GetString()), prim);
			}
		}
		std::unordered_map<std::string, std::uint32_t> firstJointByName;
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			firstJointByName.try_emplace(lower(source.jointNames[joint]), joint);
			const auto inverseBind = inverseBindsSpan[joint];
			const auto bindGlobal = DirectX::XMMatrixInverse(nullptr, inverseBind);
			DirectX::XMStoreFloat4x4(&source.inverseBindMatrices[joint], inverseBind);
			DirectX::XMStoreFloat4x4(&source.bindGlobalMatrices[joint], bindGlobal);
			const auto normalizedName = lower(source.jointNames[joint]);
			if (normalizedName.find("trunk") != std::string::npos ||
				normalizedName.find("stem") != std::string::npos ||
				normalizedName.find("root") != std::string::npos) {
				source.windSimulationGroupIndices[joint] = 0u;
			}
		}

		// BRNifly joint names refer to the exported node/Xform names. Recover the
		// nearest skinned ancestor from that hierarchy while retaining duplicate
		// skin slots (some vanilla NIFs reference the same node more than once).
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			const auto primIt = primByName.find(lower(source.jointNames[joint]));
			if (primIt == primByName.end()) continue;
			for (UsdPrim parent = primIt->second.GetParent(); parent; parent = parent.GetParent()) {
				const auto parentIt = firstJointByName.find(lower(parent.GetName().GetString()));
				if (parentIt != firstJointByName.end() && parentIt->second != joint) {
					source.parentIndices[joint] = static_cast<std::int32_t>(parentIt->second);
					break;
				}
			}
		}
		// If the asset uses unconventional names, its roots are still trunk
		// drivers and their descendants remain branch-detail drivers.
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			if (source.parentIndices[joint] < 0) source.windSimulationGroupIndices[joint] = 0u;
		}
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			const auto parent = source.parentIndices[joint];
			const auto bindGlobal = DirectX::XMLoadFloat4x4(&source.bindGlobalMatrices[joint]);
			const auto restLocal = parent >= 0
				? bindGlobal * DirectX::XMMatrixInverse(nullptr, DirectX::XMLoadFloat4x4(&source.bindGlobalMatrices[parent]))
				: bindGlobal;
			DirectX::XMStoreFloat4x4(&source.restLocalMatrices[joint], restLocal);
		}

		source.dynamicWindMetadata.bones.resize(count);
		std::vector<std::uint32_t> chainRoots(count);
		std::vector<std::uint32_t> chainDepth(count, 0u);
		std::vector<float> chainArc(count, 0.0f);
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			std::uint32_t root = joint;
			std::uint32_t depth = 0u;
			float arc = 0.0f;
			for (auto parent = source.parentIndices[root]; parent >= 0 &&
				source.windSimulationGroupIndices[parent] == source.windSimulationGroupIndices[joint];
				parent = source.parentIndices[root]) {
				const auto& a = source.bindGlobalMatrices[root];
				const auto& b = source.bindGlobalMatrices[parent];
				const float dx = a._41 - b._41, dy = a._42 - b._42, dz = a._43 - b._43;
				arc += std::sqrt(dx * dx + dy * dy + dz * dz);
				root = static_cast<std::uint32_t>(parent);
				++depth;
			}
			chainRoots[joint] = root;
			chainDepth[joint] = depth;
			chainArc[joint] = arc;
		}
		std::unordered_map<std::uint32_t, std::pair<std::uint32_t, float>> chainExtents;
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			auto& extent = chainExtents[chainRoots[joint]];
			extent.first = (std::max)(extent.first, chainDepth[joint] + 1u);
			extent.second = (std::max)(extent.second, chainArc[joint]);
		}
		for (std::uint32_t joint = 0; joint < count; ++joint) {
			auto& bone = source.dynamicWindMetadata.bones[joint];
			bone.chainOriginBoneIndex = chainRoots[joint];
			bone.indexInBoneChain = chainDepth[joint];
			bone.chainBoneCount = chainExtents[chainRoots[joint]].first;
			bone.chainLength = chainExtents[chainRoots[joint]].second;
		}

		std::string error;
		const auto artifact = SkeletonArtifactCache::Save(source, &error);
		if (!artifact) {
			spdlog::warn("NIF TREE procedural-wind skeleton generation failed for '{}': {}", sourceIdentifier, error);
			return nullptr;
		}
		auto skeleton = SkeletonArtifactCache::ResolveSkeleton(*artifact, &error);
		if (!skeleton) {
			spdlog::warn("NIF TREE procedural-wind BRSKEL resolve failed for '{}': {}", sourceIdentifier, error);
			return nullptr;
		}
		spdlog::info(
			"NIF TREE procedural-wind skeleton cached: source='{}' artifact={} joints={} lods={}.",
			sourceIdentifier, artifact->id.ToString(), artifact->jointCount, skeleton->GetSkeletonLodVariants().size());
		return skeleton;
	}

	void AttachBrNiflyTreeWindSkeletons(
		const UsdStageRefPtr& stage,
		ImportedAssetPayload& payload,
		std::string_view sourceIdentifier)
	{
		std::unordered_map<std::string, std::shared_ptr<Skeleton>> skeletonsByLayout;
		std::uint32_t metadataMeshes = 0u;
		std::uint32_t attachedMeshes = 0u;
		for (auto& mesh : payload.meshes) {
			if (!mesh || mesh->GetSkinJointNames().empty()) continue;
			++metadataMeshes;
			if (mesh->HasBaseSkin()) {
				if (mesh->GetBaseSkin()->HasWindSimulationGroups()) ++attachedMeshes;
				continue;
			}
			std::string layoutKey;
			for (const auto& name : mesh->GetSkinJointNames()) {
				layoutKey.append(name).push_back('\0');
			}
			auto& skeleton = skeletonsByLayout[layoutKey];
			if (!skeleton) skeleton = BuildBrNiflyTreeWindSkeleton(stage, *mesh, sourceIdentifier);
			if (skeleton) {
				mesh->SetBaseSkin(skeleton);
				++attachedMeshes;
			}
		}
		spdlog::info(
			"NIF TREE procedural-wind bridge: source='{}' renderableMeshes={} skinMetadataMeshes={} attachedWindMeshes={} layouts={}.",
			sourceIdentifier, payload.meshes.size(), metadataMeshes, attachedMeshes, skeletonsByLayout.size());
	}

	std::shared_ptr<Skeleton> ProcessSkeleton(const UsdSkelSkeleton& skel, const VtTokenArray rawJointOrder, const UsdSkelSkeletonQuery& skelQuery, const std::shared_ptr<Scene>& scene, double metersPerUnit) {
		if (loadingCache.skeletonMap.contains(skel.GetPrim().GetPath().GetString())) {
			spdlog::info("Skeleton {} already processed, skipping.", skel.GetPrim().GetPath().GetString());
			return loadingCache.skeletonMap[skel.GetPrim().GetPath().GetString()];
		}

		const auto& topo = skelQuery.GetTopology();
		pxr::VtArray<pxr::GfMatrix4d> bindXforms;
		skel.GetBindTransformsAttr().Get(&bindXforms);
		if (bindXforms.size() < rawJointOrder.size()) {
			spdlog::warn(
				"Skeleton '{}' bind transform count ({}) is smaller than joint count ({}); missing joints will use identity rest transforms.",
				skel.GetPrim().GetPath().GetString(),
				bindXforms.size(),
				rawJointOrder.size());
		}

		std::vector<XMMATRIX>        invBindMats;
		std::vector<flecs::entity>   jointNodes;
		invBindMats.reserve(rawJointOrder.size());
		jointNodes.reserve(rawJointOrder.size());

		for (size_t i = 0; i < rawJointOrder.size(); ++i) {
			const GfMatrix4d bindMatrix = i < bindXforms.size() ? bindXforms[i] : GfMatrix4d(1.0);
			GfMatrix4d localBindMatrix = bindMatrix;
			auto parentIdx = topo.GetParent(i);
			if (parentIdx > -1 && static_cast<size_t>(parentIdx) < bindXforms.size()) {
				localBindMatrix = bindMatrix * bindXforms[parentIdx].GetInverse();
			}
			// Convert GfMatrix4d to XMMATRIX

			// Extract translation and scale from the matrix
			auto transform = GfTransform(bindMatrix);
			auto translation = transform.GetTranslation() * metersPerUnit;
			auto rotation = transform.GetRotation().GetQuaternion();
			auto& scale = transform.GetScale();

			// Create an XMMATRIX from the translation, rotation, and scale
			XMMATRIX xm = XMMatrixScaling(static_cast<float>(scale[0]), static_cast<float>(scale[1]), static_cast<float>(scale[2])) *
				XMMatrixRotationQuaternion(XMVectorSet(static_cast<float>(rotation.GetImaginary()[0]), static_cast<float>(rotation.GetImaginary()[1]), static_cast<float>(rotation.GetImaginary()[2]), static_cast<float>(rotation.GetReal()))) *
				XMMatrixTranslation(static_cast<float>(translation[0]), static_cast<float>(translation[1]), static_cast<float>(translation[2]));
			xm = XMMatrixInverse(nullptr, xm); // Invert the matrix for the inverse bind pose

			invBindMats.push_back(xm);

			// Lookup the node by name
			std::string jn = rawJointOrder[i].GetString();
			auto it = loadingCache.nodeMap.find(jn);
			if (it != loadingCache.nodeMap.end()) {
				throw std::runtime_error("Not implemented. Does the USD spec allow this?");
			}

			auto boneNode = scene->CreateNodeECS(s2ws(jn));
			if (!boneNode.has<AnimationController>()) {
				// Create a new AnimationController for this bone
				boneNode.add<AnimationController>();
				boneNode.set<Components::AnimationName>({ jn });
			}
			SetEntityTransformFromUsdMatrix(boneNode, localBindMatrix, metersPerUnit);
			jointNodes.push_back(boneNode);
			if (parentIdx > -1) {
				boneNode.child_of(jointNodes[parentIdx]);
			}
		}

		auto skeleton = std::make_shared<Skeleton>(jointNodes, invBindMats);

		loadingCache.skeletonMap[skel.GetPrim().GetPath().GetString()] = skeleton;
		return skeleton;
	}

	std::shared_ptr<Animation> ProcessAnimQuery(const UsdSkelAnimQuery& animQuery, const UsdStageRefPtr& stage, double metersPerUnit, const VtTokenArray& jointOrder) {
		if (!animQuery) {
			return nullptr;
		}
		auto timeCodesPerSecond = stage->GetTimeCodesPerSecond();
		std::string animName = animQuery.GetPrim().GetName().GetString();
		if (loadingCache.animationMap.contains(animName)) {
			spdlog::info("Animation {} already processed, skipping.", animName);
			return loadingCache.animationMap[animName]; // Already processed
		}

		auto animation = std::make_shared<Animation>(animName);

		std::vector<double> times;
		if (!animQuery.GetJointTransformTimeSamples(&times)) {
			return animation;
		}

		for (double t : times) {
			float seconds = static_cast<float>(t / timeCodesPerSecond);
			UsdTimeCode timeCode(t);

			VtVec3fArray translations;
			VtQuatfArray rotations;
			VtVec3hArray scales;

			bool ok = animQuery.ComputeJointLocalTransformComponents(
				&translations, &rotations, &scales, timeCode);
			if (!ok) {
				continue;
			}

			for (size_t j = 0; j < jointOrder.size(); ++j) {
				const std::string nodeName = jointOrder[j].GetString();

				if (animation->nodesMap.find(nodeName) == animation->nodesMap.end()) {
					animation->nodesMap[nodeName] = std::make_shared<AnimationClip>();
				}
				auto& clip = animation->nodesMap[nodeName];

				// position
				const GfVec3f& p = translations[j] * metersPerUnit;
				clip->addPositionKeyframe(seconds,
					DirectX::XMFLOAT3(p[0], p[1], p[2]));

				// rotation
				const GfQuatf& q = rotations[j];
				const GfVec3f& i = q.GetImaginary();
				clip->addRotationKeyframe(seconds,
					XMVectorSet(i[0], i[1], i[2], q.GetReal()));

				// scale
				const GfVec3h& s = scales[j];
				clip->addScaleKeyframe(seconds,
					DirectX::XMFLOAT3(s[0], s[1], s[2]));
			}
		}

		return animation;
	}

	static std::vector<int32_t> BuildUsdSkeletonParentIndices(
		const UsdSkelTopology& topology,
		size_t jointCount)
	{
		std::vector<int32_t> parentIndices;
		parentIndices.reserve(jointCount);
		for (size_t i = 0; i < jointCount; ++i) {
			parentIndices.push_back(topology.GetParent(i));
		}
		return parentIndices;
	}

	static size_t CountUsdSkeletonRoots(const std::vector<int32_t>& parentIndices)
	{
		size_t roots = 0;
		for (const int32_t parent : parentIndices) {
			if (parent < 0) {
				++roots;
			}
		}
		return roots;
	}

	static size_t CountUsdSkeletonChildrenOf(
		const std::vector<int32_t>& parentIndices,
		int32_t parentIndex)
	{
		size_t children = 0;
		for (const int32_t parent : parentIndices) {
			if (parent == parentIndex) {
				++children;
			}
		}
		return children;
	}

	static std::string AssemblySkeletonEdgeKey(std::string_view parent, std::string_view child)
	{
		std::string key;
		key.reserve(parent.size() + child.size() + 1u);
		key.append(parent);
		key.push_back('\n');
		key.append(child);
		return key;
	}

	static void AddAssemblySkeletonEdgeHint(
		AssemblySkeletonTopologyHints& hints,
		std::string_view parent,
		std::string_view child)
	{
		if (parent.empty() || child.empty()) {
			return;
		}
		hints.naniteBindEdgeKeys.insert(AssemblySkeletonEdgeKey(parent, child));
		hints.naniteBindEdgeKeys.insert(AssemblySkeletonEdgeKey(UsdJointLeafName(parent), UsdJointLeafName(child)));
	}

	static std::string StageTopologyHintCacheKey(const UsdStageRefPtr& stage)
	{
		if (!stage) {
			return "<null>";
		}
		if (const SdfLayerHandle rootLayer = stage->GetRootLayer()) {
			return rootLayer->GetIdentifier();
		}
		return "<anonymous>";
	}

	static const AssemblySkeletonTopologyHints& GetAssemblySkeletonTopologyHints(const UsdStageRefPtr& stage)
	{
		const std::string cacheKey = StageTopologyHintCacheKey(stage);
		AssemblySkeletonTopologyHints& hints = loadingCache.assemblySkeletonTopologyHintsByStage[cacheKey];
		if (hints.scanned) {
			return hints;
		}

		hints.scanned = true;
		if (!stage) {
			return hints;
		}

		for (const UsdPrim& prim : UsdPrimRange(stage->GetPseudoRoot())) {
			// On a PointInstancer this primvar is one joint token per instance, not
			// a flattened parent/child edge list. Treating adjacent instance entries
			// as topology pairs corrupts an otherwise valid skeleton hierarchy.
			if (prim.IsA<UsdGeomPointInstancer>()) {
				continue;
			}
			const UsdAttribute bindJointsAttr = prim.GetAttribute(TfToken("primvars:unreal:naniteAssembly:bindJoints"));
			if (!bindJointsAttr) {
				continue;
			}

			VtTokenArray bindJoints;
			if (!bindJointsAttr.Get(&bindJoints) || bindJoints.size() < 2u) {
				continue;
			}

			hints.naniteBindJointPairCount += bindJoints.size() / 2u;
			for (size_t tokenIndex = 0; tokenIndex + 1u < bindJoints.size(); tokenIndex += 2u) {
				AddAssemblySkeletonEdgeHint(
					hints,
					bindJoints[tokenIndex].GetString(),
					bindJoints[tokenIndex + 1u].GetString());
			}
		}

		if (hints.naniteBindJointPairCount != 0u) {
			spdlog::info(
				"USD assembly skeleton topology hints: stage='{}' naniteBindJointPairs={} edgeKeys={}",
				cacheKey,
				hints.naniteBindJointPairCount,
				hints.naniteBindEdgeKeys.size());
		}

		return hints;
	}

	struct UsdSkeletonTopologyValidation
	{
		size_t duplicateJointNames = 0u;
		size_t missingAuthoredParents = 0u;
		size_t parentMismatches = 0u;
		size_t invalidParentIndices = 0u;
		size_t selfParents = 0u;
		size_t forwardParents = 0u;
		size_t cycles = 0u;
	};

	static UsdSkeletonTopologyValidation ValidateUsdSkeletonTopology(
		const VtTokenArray& jointOrder,
		const std::vector<int32_t>& parentIndices)
	{
		UsdSkeletonTopologyValidation validation{};
		std::unordered_map<std::string, size_t> jointIndexByName;
		jointIndexByName.reserve(jointOrder.size());
		for (size_t jointIndex = 0; jointIndex < jointOrder.size(); ++jointIndex) {
			const std::string jointName = jointOrder[jointIndex].GetString();
			if (!jointIndexByName.emplace(jointName, jointIndex).second) {
				++validation.duplicateJointNames;
			}
		}

		for (size_t jointIndex = 0; jointIndex < jointOrder.size(); ++jointIndex) {
			const int32_t parentIndex = jointIndex < parentIndices.size() ? parentIndices[jointIndex] : -1;
			if (parentIndex == static_cast<int32_t>(jointIndex)) {
				++validation.selfParents;
			}
			if (parentIndex >= static_cast<int32_t>(jointOrder.size())) {
				++validation.invalidParentIndices;
			}
			if (parentIndex > static_cast<int32_t>(jointIndex)) {
				++validation.forwardParents;
			}

			const std::string jointName = jointOrder[jointIndex].GetString();
			const size_t slash = jointName.find_last_of('/');
			if (slash == std::string::npos) {
				if (parentIndex >= 0) {
					++validation.parentMismatches;
				}
				continue;
			}

			const std::string authoredParentName = jointName.substr(0u, slash);
			const auto authoredParentIt = jointIndexByName.find(authoredParentName);
			if (authoredParentIt == jointIndexByName.end()) {
				++validation.missingAuthoredParents;
			}
			else if (parentIndex != static_cast<int32_t>(authoredParentIt->second)) {
				++validation.parentMismatches;
			}
		}

		std::vector<uint8_t> visitState(jointOrder.size(), 0u);
		std::function<bool(size_t)> visit = [&](size_t jointIndex) -> bool {
			if (jointIndex >= parentIndices.size()) {
				return false;
			}
			if (visitState[jointIndex] == 1u) {
				return true;
			}
			if (visitState[jointIndex] == 2u) {
				return false;
			}
			visitState[jointIndex] = 1u;
			const int32_t parentIndex = parentIndices[jointIndex];
			bool hasCycle = false;
			if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < jointOrder.size()) {
				hasCycle = visit(static_cast<size_t>(parentIndex));
			}
			visitState[jointIndex] = 2u;
			return hasCycle;
		};
		for (size_t jointIndex = 0; jointIndex < jointOrder.size(); ++jointIndex) {
			if (visit(jointIndex)) {
				++validation.cycles;
			}
		}

		return validation;
	}

	static int32_t FindNearestPriorJoint(
		const std::vector<GfVec3d>& jointPositions,
		size_t jointIndex,
		int32_t excludedJoint,
		double* outDistance = nullptr)
	{
		int32_t bestParent = -1;
		double bestDistance = std::numeric_limits<double>::max();
		for (size_t candidate = 0; candidate < jointIndex; ++candidate) {
			if (static_cast<int32_t>(candidate) == excludedJoint) {
				continue;
			}
			const double distance = UsdDistance(jointPositions[jointIndex], jointPositions[candidate]);
			if (distance < bestDistance) {
				bestDistance = distance;
				bestParent = static_cast<int32_t>(candidate);
			}
		}
		if (outDistance) {
			*outDistance = bestDistance;
		}
		return bestParent;
	}

	static bool SanitizeAssemblySkeletonParentIndices(
		const std::string& skeletonPath,
		const VtTokenArray& jointOrder,
		const VtArray<GfMatrix4d>& bindXforms,
		const AssemblySkeletonTopologyHints& hints,
		std::vector<int32_t>& parentIndices)
	{
		const size_t jointCount = jointOrder.size();
		if (jointCount < 3 || bindXforms.empty() || parentIndices.size() != jointCount) {
			return true;
		}

		int32_t primaryRoot = -1;
		for (size_t i = 0; i < parentIndices.size(); ++i) {
			if (parentIndices[i] < 0) {
				primaryRoot = static_cast<int32_t>(i);
				break;
			}
		}
		if (primaryRoot < 0) {
			return true;
		}

		std::vector<GfVec3d> jointPositions(jointCount, GfVec3d(0.0));
		for (size_t i = 0; i < jointCount; ++i) {
			const GfMatrix4d bindMatrix = i < bindXforms.size() ? bindXforms[i] : GfMatrix4d(1.0);
			jointPositions[i] = ExtractUsdMatrixTranslation(bindMatrix);
		}

		const size_t rootsBefore = CountUsdSkeletonRoots(parentIndices);
		const size_t rootChildrenBefore = CountUsdSkeletonChildrenOf(parentIndices, primaryRoot);
		const std::vector<int32_t> authoredParentIndices = parentIndices;
		const UsdSkeletonTopologyValidation authoredValidation = ValidateUsdSkeletonTopology(jointOrder, parentIndices);
		const bool authoredTopologyValid =
			authoredValidation.duplicateJointNames == 0u &&
			authoredValidation.missingAuthoredParents == 0u &&
			authoredValidation.parentMismatches == 0u &&
			authoredValidation.invalidParentIndices == 0u &&
			authoredValidation.selfParents == 0u &&
			authoredValidation.forwardParents == 0u &&
			authoredValidation.cycles == 0u;
		if (authoredTopologyValid) {
			spdlog::info(
				"USD assembly skeleton topology '{}': authored hierarchy is valid; preserving all {} parent links.",
				skeletonPath,
				jointCount - CountUsdSkeletonRoots(parentIndices));
			return true;
		}

		enum class SanitizedParentReason
		{
			NaniteAssemblyBindEdge,
			CoincidentBindPoseAlias,
			PropagatedThroughSanitizedParent,
		};

		struct SanitizedParentEdge
		{
			size_t jointIndex = 0;
			int32_t oldParent = -1;
			int32_t newParent = -1;
			double oldDistance = 0.0;
			double newDistance = 0.0;
			SanitizedParentReason reason = SanitizedParentReason::NaniteAssemblyBindEdge;
		};

		std::vector<uint8_t> sanitizedJoint(jointCount, 0u);
		std::vector<SanitizedParentEdge> sanitizedEdges;
		size_t naniteEdgeCount = 0u;
		size_t coincidentAliasCount = 0u;
		size_t propagatedEdgeCount = 0u;
		constexpr double kCoincidentBindPositionEpsilon = 1.0e-8;

		for (size_t i = 0; i < jointCount; ++i) {
			if (static_cast<int32_t>(i) == primaryRoot) {
				continue;
			}

			const int32_t oldParent = parentIndices[i];
			if (oldParent < 0 || static_cast<size_t>(oldParent) >= jointPositions.size()) {
				continue;
			}

			const double oldDistance = UsdDistance(jointPositions[i], jointPositions[static_cast<size_t>(oldParent)]);
			const std::string childName = jointOrder[i].GetString();
			const std::string parentName = jointOrder[static_cast<size_t>(oldParent)].GetString();
			const bool isNaniteBindEdge =
				hints.naniteBindEdgeKeys.contains(AssemblySkeletonEdgeKey(parentName, childName)) ||
				hints.naniteBindEdgeKeys.contains(AssemblySkeletonEdgeKey(UsdJointLeafName(parentName), UsdJointLeafName(childName)));

			double nearestDistance = std::numeric_limits<double>::max();
			const int32_t nearestParent = FindNearestPriorJoint(jointPositions, i, isNaniteBindEdge ? oldParent : -1, &nearestDistance);
			int32_t newParent = oldParent;
			SanitizedParentReason reason = SanitizedParentReason::NaniteAssemblyBindEdge;

			if (isNaniteBindEdge && nearestParent >= 0) {
				newParent = nearestParent;
				reason = SanitizedParentReason::NaniteAssemblyBindEdge;
			}
			else if (nearestParent >= 0 &&
				nearestParent != oldParent &&
				nearestDistance <= kCoincidentBindPositionEpsilon &&
				oldDistance > kCoincidentBindPositionEpsilon) {
				newParent = nearestParent;
				reason = SanitizedParentReason::CoincidentBindPoseAlias;
			}
			else if (sanitizedJoint[static_cast<size_t>(oldParent)] != 0u) {
				const int32_t authoredGrandparent = authoredParentIndices[static_cast<size_t>(oldParent)];
				if (authoredGrandparent >= 0 &&
					static_cast<size_t>(authoredGrandparent) < i &&
					authoredGrandparent != oldParent) {
					const double grandparentDistance =
						UsdDistance(jointPositions[i], jointPositions[static_cast<size_t>(authoredGrandparent)]);
					if (grandparentDistance + kCoincidentBindPositionEpsilon < oldDistance) {
						newParent = authoredGrandparent;
						nearestDistance = grandparentDistance;
						reason = SanitizedParentReason::PropagatedThroughSanitizedParent;
					}
				}
			}

			if (newParent < 0 || newParent == oldParent || static_cast<size_t>(newParent) >= i) {
				continue;
			}

			parentIndices[i] = newParent;
			sanitizedJoint[i] = 1u;
			switch (reason) {
			case SanitizedParentReason::NaniteAssemblyBindEdge:
				++naniteEdgeCount;
				break;
			case SanitizedParentReason::CoincidentBindPoseAlias:
				++coincidentAliasCount;
				break;
			case SanitizedParentReason::PropagatedThroughSanitizedParent:
				++propagatedEdgeCount;
				break;
			}
			sanitizedEdges.push_back({ i, oldParent, newParent, oldDistance, nearestDistance, reason });
		}

		const size_t rootsAfter = CountUsdSkeletonRoots(parentIndices);
		const size_t rootChildrenAfter = CountUsdSkeletonChildrenOf(parentIndices, primaryRoot);
		const UsdSkeletonTopologyValidation sanitizedValidation = ValidateUsdSkeletonTopology(jointOrder, parentIndices);
		const bool sanitizedTopologyValid =
			authoredValidation.duplicateJointNames == 0u &&
			sanitizedValidation.invalidParentIndices == 0u &&
			sanitizedValidation.selfParents == 0u &&
			sanitizedValidation.forwardParents == 0u &&
			sanitizedValidation.cycles == 0u;
		spdlog::info(
			"USD assembly skeleton topology sanitize '{}': joints={}, roots {}->{}, rootChildren {}->{}, naniteBindPairs={}, sanitized={} (nanite={}, coincident={}, propagated={}), authoredIssues={{duplicates={}, missingParents={}, parentMismatches={}, invalidParents={}, cycles={}}}, sanitizedIssues={{invalidParents={}, selfParents={}, forwardParents={}, cycles={}}}",
			skeletonPath,
			jointCount,
			rootsBefore,
			rootsAfter,
			rootChildrenBefore,
			rootChildrenAfter,
			hints.naniteBindJointPairCount,
			sanitizedEdges.size(),
			naniteEdgeCount,
			coincidentAliasCount,
			propagatedEdgeCount,
			authoredValidation.duplicateJointNames,
			authoredValidation.missingAuthoredParents,
			authoredValidation.parentMismatches,
			authoredValidation.invalidParentIndices,
			authoredValidation.cycles,
			sanitizedValidation.invalidParentIndices,
			sanitizedValidation.selfParents,
			sanitizedValidation.forwardParents,
			sanitizedValidation.cycles);
		if (!sanitizedTopologyValid) {
			spdlog::error(
				"USD assembly skeleton topology sanitize '{}': refusing invalid hierarchy after sanitation.",
				skeletonPath);
			return false;
		}

		auto reasonName = [](SanitizedParentReason reason) -> const char* {
			switch (reason) {
			case SanitizedParentReason::NaniteAssemblyBindEdge:
				return "naniteBindEdge";
			case SanitizedParentReason::CoincidentBindPoseAlias:
				return "coincidentBindAlias";
			case SanitizedParentReason::PropagatedThroughSanitizedParent:
				return "propagatedThroughSanitizedParent";
			}
			return "unknown";
		};
		const size_t detailCount = (std::min<size_t>)(sanitizedEdges.size(), 24u);
		for (size_t i = 0; i < detailCount; ++i) {
			const auto& edge = sanitizedEdges[i];
			const std::string_view oldParentName = edge.oldParent >= 0 && static_cast<size_t>(edge.oldParent) < jointOrder.size()
				? UsdJointLeafName(jointOrder[static_cast<size_t>(edge.oldParent)].GetString())
				: std::string_view("<root>");
			const std::string_view newParentName = edge.newParent >= 0 && static_cast<size_t>(edge.newParent) < jointOrder.size()
				? UsdJointLeafName(jointOrder[static_cast<size_t>(edge.newParent)].GetString())
				: std::string_view("<root>");
			spdlog::info(
				"  USD assembly skeleton sanitized '{}': '{}' -> '{}' reason={} (distance {:.4f} -> {:.4f})",
				UsdJointLeafName(jointOrder[edge.jointIndex].GetString()),
				oldParentName,
				newParentName,
				reasonName(edge.reason),
				edge.oldDistance,
				edge.newDistance);
		}
		if (sanitizedEdges.size() > detailCount) {
			spdlog::info(
				"  USD assembly skeleton '{}': {} additional sanitized parent links omitted",
				skeletonPath,
				sanitizedEdges.size() - detailCount);
		}
		return true;
	}

	std::shared_ptr<Skeleton> BuildPayloadSkeleton(
		const UsdSkelSkeleton& skel,
		const VtTokenArray& rawJointOrder,
		const UsdSkelSkeletonQuery& skelQuery,
		double metersPerUnit,
		bool sanitizeAssemblyHierarchy,
		const UsdStageRefPtr& stage)
	{
		ZoneScopedN("USDLoader::BuildPayloadSkeleton");
		const auto skeletonPath = skel.GetPrim().GetPath().GetString();
		ZoneText(skeletonPath.data(), skeletonPath.size());
		const AssemblySkeletonTopologyHints* topologyHints = sanitizeAssemblyHierarchy
			? &GetAssemblySkeletonTopologyHints(stage)
			: nullptr;
		const std::string skeletonCacheKey = sanitizeAssemblyHierarchy
			? skeletonPath + "#assembly_topology_sanitize=2#nanite_pairs=" +
				std::to_string(topologyHints ? topologyHints->naniteBindJointPairCount : 0u)
			: skeletonPath;
		if (loadingCache.skeletonMap.contains(skeletonCacheKey)) {
			return loadingCache.skeletonMap[skeletonCacheKey];
		}

		const auto& topology = skelQuery.GetTopology();
		pxr::VtArray<pxr::GfMatrix4d> bindXforms;
		skel.GetBindTransformsAttr().Get(&bindXforms);
		if (bindXforms.size() < rawJointOrder.size()) {
			spdlog::warn(
				"Skeleton '{}' bind transform count ({}) is smaller than joint count ({}); missing joints will use identity bind transforms.",
				skel.GetPrim().GetPath().GetString(),
				bindXforms.size(),
				rawJointOrder.size());
		}

		std::vector<std::string> boneNames;
		std::vector<int32_t> parentIndices;
		std::vector<DirectX::XMMATRIX> inverseBindMatrices;
		std::vector<Components::Transform> restLocalTransforms;
		boneNames.reserve(rawJointOrder.size());
		parentIndices.reserve(rawJointOrder.size());
		inverseBindMatrices.reserve(rawJointOrder.size());
		restLocalTransforms.reserve(rawJointOrder.size());

		parentIndices = BuildUsdSkeletonParentIndices(topology, rawJointOrder.size());
		if (sanitizeAssemblyHierarchy && topologyHints != nullptr) {
			if (!SanitizeAssemblySkeletonParentIndices(skeletonPath, rawJointOrder, bindXforms, *topologyHints, parentIndices)) {
				return nullptr;
			}
		}

		PayloadSkeletonBuildMetadata metadata;
		metadata.skeletonPath = skeletonPath;
		metadata.parentIndices = parentIndices;
		metadata.metersPerUnit = metersPerUnit;
		metadata.boneNames.reserve(rawJointOrder.size());
		metadata.bindXforms.reserve(rawJointOrder.size());
		metadata.windSimulationGroupIndices.assign(rawJointOrder.size(), 0xFFFFFFFFu);
		metadata.dynamicWindMetadata.bones.resize(rawJointOrder.size());
		if (stage && stage->GetRootLayer()) {
			metadata.windProfileIdentity = stage->GetRootLayer()->GetIdentifier();
		}
		{
			bool loadedDynamicWindJson = false;
			const UsdSkelRoot skelRoot = UsdSkelRoot::Find(skel.GetPrim());
			std::string dynamicWindJson;
			if (skelRoot && skelRoot.GetPrim().GetAttribute(TfToken("unreal:dynamicWind:data")).Get(&dynamicWindJson) && !dynamicWindJson.empty()) {
				try {
					const json root = json::parse(dynamicWindJson);
					auto& wind = metadata.dynamicWindMetadata;
					wind.enabled = root.value("bIsEnabled", false);
					wind.groundCover = root.value("bIsGroundCover", false);
					wind.gustAttenuation = root.value("gustAttenuation", 0.0f);
					wind.attachedBranchProfileGroupId = root.value("attachedBranchProfileGroupId", 1u);
					wind.maximumLodVariants = std::clamp(root.value("skeletonLodMaxVariants", 16u), 2u, 16u);
					wind.maximumAdjacentBoneRatio = std::clamp(root.value("skeletonLodMaximumBoneRatio", 1.75f), 1.05f, 8.0f);
					wind.skeletonLodQualityBias = std::clamp(root.value("skeletonLodQualityBias", 1.0f), 0.0f, 4.0f);
					if (const auto it = root.find("skeletonLodTargetBoneCounts"); it != root.end() && it->is_array())
						for (const auto& value : *it) if (value.is_number_unsigned()) wind.skeletonLodTargetBoneCounts.push_back(value.get<std::uint32_t>());
					if (const auto it = root.find("simulationGroups"); it != root.end() && it->is_array()) {
						for (const auto& entry : *it) {
							DynamicWindSimulationGroupData group;
							if (entry.value("bUseDualInfluence", false)) group.flags |= DynamicWindMetadata::GroupFlagDualInfluence;
							if (entry.value("bIsTrunkGroup", false)) group.flags |= DynamicWindMetadata::GroupFlagTrunk;
							group.role = (group.flags & DynamicWindMetadata::GroupFlagTrunk) != 0u
								? DynamicWindSimulationGroupRole::Trunk
								: DynamicWindSimulationGroupRole::DetailBranch;
							group.profileGroupId = entry.value("profileGroupId", static_cast<std::uint32_t>(wind.groups.size()));
							group.reductionPriority = entry.value(
								"skeletonLodReductionPriority",
								group.role == DynamicWindSimulationGroupRole::Trunk ? 4.0f : 2.0f);
							group.minimumDriverCount = entry.value(
								"skeletonLodMinimumDriverCount",
								group.role == DynamicWindSimulationGroupRole::Trunk ? 2u : 0u);
							const std::string role = entry.value("skeletonLodRole", std::string{});
							if (role == "trunk") group.role = DynamicWindSimulationGroupRole::Trunk;
							else if (role == "detailBranch") group.role = DynamicWindSimulationGroupRole::DetailBranch;
							else if (role == "attachedBranch") group.role = DynamicWindSimulationGroupRole::AttachedBranch;
							group.influence = entry.value("influence", 1.0f);
							group.minInfluence = entry.value("minInfluence", 0.0f);
							group.maxInfluence = entry.value("maxInfluence", 0.0f);
							group.shiftTop = entry.value("shiftTop", 0.0f);
							wind.groups.push_back(group);
						}
					}
					if (const auto it = root.find("simulationGroupBones"); it != root.end() && it->is_array()) {
						for (const auto& entry : *it) {
							const int group = entry.value("simulationGroupIndex", -1);
							if (const auto bones = entry.find("boneIndices"); bones != entry.end() && bones->is_array()) {
								for (const auto& bone : *bones) {
									const auto index = bone.get<std::size_t>();
									if (index < metadata.windSimulationGroupIndices.size())
										metadata.windSimulationGroupIndices[index] = group >= 0 ? static_cast<uint32_t>(group) : 0xFFFFFFFFu;
								}
							}
						}
					}
					if (const auto it = root.find("boneChains"); it != root.end() && it->is_object()) {
						for (auto chain = it->begin(); chain != it->end(); ++chain) {
							const auto origin = static_cast<std::size_t>(std::stoull(chain.key()));
							if (origin < wind.bones.size()) {
								wind.bones[origin].chainOriginBoneIndex = static_cast<uint32_t>(origin);
								wind.bones[origin].chainBoneCount = chain.value().value("numBones", 0u);
								wind.bones[origin].chainLength = chain.value().value("chainLength", 0.0f);
							}
						}
					}
					if (const auto it = root.find("extraBonesData"); it != root.end() && it->is_object()) {
						for (auto bone = it->begin(); bone != it->end(); ++bone) {
							const auto index = static_cast<std::size_t>(std::stoull(bone.key()));
							if (index < wind.bones.size()) {
								wind.bones[index].chainOriginBoneIndex = bone.value().value("boneChainOriginBoneIndex", 0xFFFFFFFFu);
								wind.bones[index].indexInBoneChain = bone.value().value("indexInBoneChain", 0u);
							}
						}
					}
					for (auto& bone : wind.bones) {
						if (bone.chainOriginBoneIndex < wind.bones.size() && bone.chainOriginBoneIndex != 0xFFFFFFFFu) {
							const auto& origin = wind.bones[bone.chainOriginBoneIndex];
							bone.chainBoneCount = origin.chainBoneCount;
							bone.chainLength = origin.chainLength;
						}
					}
					loadedDynamicWindJson = true;
					const size_t matched = std::ranges::count_if(metadata.windSimulationGroupIndices, [](uint32_t group) { return group != 0xFFFFFFFFu; });
					spdlog::info("Skeleton '{}' DynamicWind JSON metadata: groups={} matched={} chains={}.", skeletonPath, wind.groups.size(), matched, root.value("boneChains", json::object()).size());
				}
				catch (const std::exception& e) {
					spdlog::warn("Skeleton '{}' has invalid DynamicWind JSON: {}", skeletonPath, e.what());
				}
			}
			VtTokenArray windJointNames;
			VtIntArray windGroups;
			const bool hasNames = skel.GetPrim().GetAttribute(TfToken("unreal:dynamicWind:jointNames")).Get(&windJointNames);
			const bool hasGroups = skel.GetPrim().GetAttribute(TfToken("unreal:dynamicWind:jointSimulationGroups")).Get(&windGroups);
			if ((hasNames || hasGroups) && (!hasNames || !hasGroups || windJointNames.size() != windGroups.size())) {
				spdlog::warn("Skeleton '{}' has invalid DynamicWind arrays (names={}, groups={}); wind metadata disabled.",
					skeletonPath, windJointNames.size(), windGroups.size());
			}
			else if (!loadedDynamicWindJson && hasNames && hasGroups) {
				std::unordered_map<std::string, uint32_t> groupByName;
				groupByName.reserve(windJointNames.size());
				size_t duplicateNames = 0;
				for (size_t i = 0; i < windJointNames.size(); ++i) {
					const auto [_, inserted] = groupByName.try_emplace(
						windJointNames[i].GetString(),
						windGroups[i] >= 0 ? static_cast<uint32_t>(windGroups[i]) : 0xFFFFFFFFu);
					duplicateNames += inserted ? 0u : 1u;
				}
				for (size_t i = 0; i < rawJointOrder.size(); ++i) {
					const std::string authored = rawJointOrder[i].GetString();
					auto found = groupByName.find(authored);
					if (found == groupByName.end()) {
						const size_t slash = authored.find_last_of('/');
						found = groupByName.find(slash == std::string::npos ? authored : authored.substr(slash + 1u));
					}
					if (found != groupByName.end()) metadata.windSimulationGroupIndices[i] = found->second;
				}
				const size_t matched = std::ranges::count_if(metadata.windSimulationGroupIndices, [](uint32_t group) { return group != 0xFFFFFFFFu; });
				spdlog::info("Skeleton '{}' DynamicWind metadata: authored={} matched={}.", skeletonPath, windJointNames.size(), matched);
				if (duplicateNames != 0u || matched != rawJointOrder.size()) {
					spdlog::warn("Skeleton '{}' DynamicWind mapping had {} duplicate authored names and {} unmatched USD joints.",
						skeletonPath, duplicateNames, rawJointOrder.size() - matched);
				}
			}
		}

		for (size_t i = 0; i < rawJointOrder.size(); ++i) {
			boneNames.push_back(rawJointOrder[i].GetString());
			metadata.boneNames.push_back(boneNames.back());
			const int parentIndex = i < parentIndices.size() ? parentIndices[i] : -1;

			const GfMatrix4d bindMatrix = i < bindXforms.size() ? bindXforms[i] : GfMatrix4d(1.0);
			metadata.bindXforms.push_back(bindMatrix);
			GfMatrix4d localBindMatrix = bindMatrix;
			if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < bindXforms.size()) {
				localBindMatrix = bindMatrix * bindXforms[static_cast<size_t>(parentIndex)].GetInverse();
			}
			restLocalTransforms.push_back(ComponentsTransformFromUsdMatrix(localBindMatrix, metersPerUnit));
			inverseBindMatrices.push_back(DirectX::XMMatrixInverse(nullptr, DirectXMatrixFromUsdMatrix(bindMatrix, metersPerUnit)));
		}

		auto skeleton = std::make_shared<Skeleton>(
			std::move(boneNames),
			std::move(parentIndices),
			std::move(inverseBindMatrices),
			std::move(restLocalTransforms),
			std::vector<DirectX::XMMATRIX>{},
			metadata.windSimulationGroupIndices,
			metadata.windProfileIdentity,
			metadata.dynamicWindMetadata);
		loadingCache.skeletonMap[skeletonCacheKey] = skeleton;
		loadingCache.payloadSkeletonMetadata[skeleton.get()] = std::move(metadata);
		return skeleton;
	}

	void AddPayloadSkeletonAnimation(
		const UsdSkelSkeleton& skel,
		UsdSkelCache& skelCache,
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		const VtTokenArray& rawJointOrder,
		const std::shared_ptr<Skeleton>& skeleton)
	{
		if (!skel || !stage || !skeleton || skeleton->GetAnimationCount() > 0u) {
			return;
		}

		UsdSkelBindingAPI skelAPI(skel.GetPrim());
		UsdPrim animPrim;
		if (!skelAPI.GetAnimationSource(&animPrim)) {
			return;
		}

		UsdSkelAnimation anim(animPrim);
		auto animQuery = skelCache.GetAnimQuery(anim);
		if (!animQuery) {
			return;
		}

		if (auto animation = ProcessAnimQuery(animQuery, stage, metersPerUnit, rawJointOrder)) {
			skeleton->AddAnimation(animation);
		}
	}

}
