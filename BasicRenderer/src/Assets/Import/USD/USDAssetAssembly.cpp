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
#include <BasicRenderer/Assets/Import/USDMaterialCache.h>
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.h"
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

#include "Assets/Import/USD/USDAssetAssembly.h"
#include "Assets/Import/USD/USDGeometryConversion.h"
#include "Assets/Import/USD/USDSkeletonConversion.h"
#include "Assets/Import/USD/USDStageHelpers.h"

namespace USDLoader {
	using namespace pxr;
	using json = nlohmann::json;

	static void StoreMatrix4x4(DirectX::XMFLOAT4X4& out, DirectX::XMMATRIX matrix)
	{
		DirectX::XMStoreFloat4x4(&out, matrix);
	}

	static Components::Transform ComponentsTransformFromDirectXMatrix(DirectX::XMMATRIX matrix)
	{
		DirectX::XMVECTOR scale = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
		DirectX::XMVECTOR rotation = DirectX::XMQuaternionIdentity();
		DirectX::XMVECTOR translation = DirectX::XMVectorZero();
		if (!DirectX::XMMatrixDecompose(&scale, &rotation, &translation, matrix)) {
			scale = DirectX::XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
			rotation = DirectX::XMQuaternionIdentity();
			translation = DirectX::XMVectorZero();
		}
		return Components::Transform(
			Components::Position(translation),
			Components::Rotation(rotation),
			Components::Scale(scale));
	}

	static DirectX::XMFLOAT3 ExtractDirectXMatrixTranslation(DirectX::XMMATRIX matrix)
	{
		DirectX::XMFLOAT3 translation{};
		DirectX::XMStoreFloat3(
			&translation,
			DirectX::XMVector3TransformCoord(DirectX::XMVectorZero(), matrix));
		return translation;
	}

	static float Distance(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b)
	{
		const float x = a.x - b.x;
		const float y = a.y - b.y;
		const float z = a.z - b.z;
		return std::sqrt(x * x + y * y + z * z);
	}

	static void LogAssemblySkeletonTopologyDiagnostics(
		const ClusterLODAssemblySkeletonData& data,
		std::string_view context)
	{
		if (data.Empty()) {
			return;
		}

		const size_t jointCount = data.jointNames.size();
		std::vector<DirectX::XMFLOAT3> bindPositions;
		bindPositions.reserve(jointCount);
		for (size_t jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
			if (jointIndex < data.bindGlobalMatrices.size()) {
				bindPositions.push_back(ExtractDirectXMatrixTranslation(DirectX::XMLoadFloat4x4(&data.bindGlobalMatrices[jointIndex])));
			}
			else if (jointIndex < data.restLocalMatrices.size()) {
				bindPositions.push_back(ExtractDirectXMatrixTranslation(DirectX::XMLoadFloat4x4(&data.restLocalMatrices[jointIndex])));
			}
			else {
				bindPositions.push_back(DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f });
			}
		}

		struct ParentEdgeDiagnostic
		{
			size_t child = 0;
			size_t parent = 0;
			float distance = 0.0f;
		};

		auto leafName = [](const std::string& name) {
			const std::string_view view(name);
			const size_t slash = view.find_last_of('/');
			return slash == std::string_view::npos ? view : view.substr(slash + 1u);
		};
		auto skeletonInstancePrefix = [](const std::string& name) {
			const std::string_view view(name);
			const size_t bracket = view.find(']');
			return bracket == std::string_view::npos ? view : view.substr(0u, bracket + 1u);
		};

		size_t rootCount = 0;
		size_t parentLineCount = 0;
		size_t invalidParentCount = 0;
		size_t edgesToJointZero = 0;
		size_t longEdgesFromRootRegionCount = 0;
		size_t crossSkeletonInstanceEdgeCount = 0;
		size_t longCrossSkeletonInstanceEdgeCount = 0;
		std::vector<ParentEdgeDiagnostic> longEdges;
		std::vector<ParentEdgeDiagnostic> jointZeroEdges;
		std::vector<ParentEdgeDiagnostic> longEdgesFromRootRegion;
		std::vector<ParentEdgeDiagnostic> longCrossSkeletonInstanceEdges;
		const DirectX::XMFLOAT3 assemblyRootPosition = bindPositions.empty()
			? DirectX::XMFLOAT3{ 0.0f, 0.0f, 0.0f }
			: bindPositions[0];
		constexpr float kRootRegionRadius = 2.0f;
		constexpr float kLongRootEdgeDistance = 4.0f;
		for (size_t jointIndex = 0; jointIndex < jointCount; ++jointIndex) {
			const int32_t parent = jointIndex < data.parentIndices.size()
				? data.parentIndices[jointIndex]
				: -1;
			if (parent < 0) {
				++rootCount;
				continue;
			}
			if (static_cast<size_t>(parent) >= jointCount) {
				++invalidParentCount;
				continue;
			}

			++parentLineCount;
			const float distance = Distance(bindPositions[jointIndex], bindPositions[static_cast<size_t>(parent)]);
			longEdges.push_back(ParentEdgeDiagnostic{
				.child = jointIndex,
				.parent = static_cast<size_t>(parent),
				.distance = distance,
			});
			if (parent == 0 && jointIndex != 0u) {
				++edgesToJointZero;
				jointZeroEdges.push_back(ParentEdgeDiagnostic{
					.child = jointIndex,
					.parent = 0u,
					.distance = distance,
				});
			}
			const float parentDistanceToRoot = Distance(bindPositions[static_cast<size_t>(parent)], assemblyRootPosition);
			if (parentDistanceToRoot <= kRootRegionRadius && distance >= kLongRootEdgeDistance) {
				++longEdgesFromRootRegionCount;
				longEdgesFromRootRegion.push_back(ParentEdgeDiagnostic{
					.child = jointIndex,
					.parent = static_cast<size_t>(parent),
					.distance = distance,
				});
			}
			if (skeletonInstancePrefix(data.jointNames[jointIndex]) !=
				skeletonInstancePrefix(data.jointNames[static_cast<size_t>(parent)])) {
				++crossSkeletonInstanceEdgeCount;
				if (distance >= kLongRootEdgeDistance) {
					++longCrossSkeletonInstanceEdgeCount;
					longCrossSkeletonInstanceEdges.push_back(ParentEdgeDiagnostic{
						.child = jointIndex,
						.parent = static_cast<size_t>(parent),
						.distance = distance,
					});
				}
			}
		}

		std::sort(longEdges.begin(), longEdges.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.distance > rhs.distance;
		});
		std::sort(jointZeroEdges.begin(), jointZeroEdges.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.distance > rhs.distance;
		});
		std::sort(longEdgesFromRootRegion.begin(), longEdgesFromRootRegion.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.distance > rhs.distance;
		});
		std::sort(longCrossSkeletonInstanceEdges.begin(), longCrossSkeletonInstanceEdges.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.distance > rhs.distance;
		});

		spdlog::info(
			"USD CLod assembly skeleton topology [{}]: joints={} roots={} parentLines={} edgesToJoint0={} longRootRegionEdges={} crossInstanceEdges={} longCrossInstanceEdges={} invalidParents={} rootParentGlobalLinesExpected=0",
			context,
			jointCount,
			rootCount,
			parentLineCount,
			edgesToJointZero,
			longEdgesFromRootRegionCount,
			crossSkeletonInstanceEdgeCount,
			longCrossSkeletonInstanceEdgeCount,
			invalidParentCount);

		const size_t jointZeroDetailCount = (std::min<size_t>)(jointZeroEdges.size(), 16u);
		for (size_t i = 0; i < jointZeroDetailCount; ++i) {
			const ParentEdgeDiagnostic& edge = jointZeroEdges[i];
			spdlog::info(
				"  assembly skeleton edge-to-joint0 [{}]: child={} '{}' parent=0 '{}' distance={:.4f}",
				context,
				edge.child,
				leafName(data.jointNames[edge.child]),
				leafName(data.jointNames[0]),
				edge.distance);
		}
		if (jointZeroEdges.size() > jointZeroDetailCount) {
			spdlog::info(
				"  assembly skeleton edge-to-joint0 [{}]: {} additional edges omitted",
				context,
				jointZeroEdges.size() - jointZeroDetailCount);
		}

		const size_t longEdgeDetailCount = (std::min<size_t>)(longEdges.size(), 12u);
		for (size_t i = 0; i < longEdgeDetailCount; ++i) {
			const ParentEdgeDiagnostic& edge = longEdges[i];
			spdlog::info(
				"  assembly skeleton longest edge [{}]: child={} '{}' parent={} '{}' distance={:.4f}",
				context,
				edge.child,
				leafName(data.jointNames[edge.child]),
				edge.parent,
				leafName(data.jointNames[edge.parent]),
				edge.distance);
		}

		const size_t rootRegionDetailCount = (std::min<size_t>)(longEdgesFromRootRegion.size(), 24u);
		for (size_t i = 0; i < rootRegionDetailCount; ++i) {
			const ParentEdgeDiagnostic& edge = longEdgesFromRootRegion[i];
			spdlog::info(
				"  assembly skeleton long edge from root-region [{}]: child={} '{}' parent={} '{}' distance={:.4f}",
				context,
				edge.child,
				leafName(data.jointNames[edge.child]),
				edge.parent,
				leafName(data.jointNames[edge.parent]),
				edge.distance);
		}
		if (longEdgesFromRootRegion.size() > rootRegionDetailCount) {
			spdlog::info(
				"  assembly skeleton long edge from root-region [{}]: {} additional edges omitted",
				context,
				longEdgesFromRootRegion.size() - rootRegionDetailCount);
		}

		const size_t crossInstanceDetailCount = (std::min<size_t>)(longCrossSkeletonInstanceEdges.size(), 24u);
		for (size_t i = 0; i < crossInstanceDetailCount; ++i) {
			const ParentEdgeDiagnostic& edge = longCrossSkeletonInstanceEdges[i];
			spdlog::info(
				"  assembly skeleton long cross-instance edge [{}]: child={} '{}' parent={} '{}' distance={:.4f}",
				context,
				edge.child,
				leafName(data.jointNames[edge.child]),
				edge.parent,
				leafName(data.jointNames[edge.parent]),
				edge.distance);
		}
		if (longCrossSkeletonInstanceEdges.size() > crossInstanceDetailCount) {
			spdlog::info(
				"  assembly skeleton long cross-instance edge [{}]: {} additional edges omitted",
				context,
				longCrossSkeletonInstanceEdges.size() - crossInstanceDetailCount);
		}
	}

	static std::uint64_t StableHashString64(const std::string& value)
	{
		std::uint64_t hash = 1469598103934665603ull;
		for (const unsigned char c : value) {
			hash ^= static_cast<std::uint64_t>(c);
			hash *= 1099511628211ull;
		}
		return hash;
	}

	static void StableHashCombine64(std::uint64_t& seed, std::uint64_t value)
	{
		seed ^= value + 0x9E3779B97F4A7C15ull + (seed << 6u) + (seed >> 2u);
	}

	static std::uint64_t StableHashDouble64(double value)
	{
		std::uint64_t bits = 0u;
		static_assert(sizeof(bits) == sizeof(value));
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static void StableHashMatrix64(std::uint64_t& seed, const GfMatrix4d& matrix)
	{
		for (int row = 0; row < 4; ++row) {
			for (int column = 0; column < 4; ++column) {
				StableHashCombine64(seed, StableHashDouble64(matrix[row][column]));
			}
		}
	}

	static std::string AssetAssemblyMaterialDomainPath(const UsdShadeMaterial& material)
	{
		return material ? material.GetPrim().GetPath().GetString() : std::string("<unbound>");
	}

	static bool MeshHasUsdSkinningPrimvars(const UsdGeomMesh& mesh)
	{
		if (!mesh) {
			return false;
		}

		UsdSkelBindingAPI bindAPI(mesh.GetPrim());
		return bindAPI.GetJointIndicesPrimvar() || bindAPI.GetJointWeightsPrimvar();
	}

	static std::uint64_t ComputeUsdSkeletonDomainHash(
		const UsdSkelSkeleton& skel,
		const VtTokenArray& jointOrder,
		const UsdSkelSkeletonQuery& skelQuery)
	{
		std::uint64_t hash = StableHashString64(skel.GetPrim().GetPath().GetString());
		const auto& topology = skelQuery.GetTopology();

		VtArray<GfMatrix4d> bindTransforms;
		skel.GetBindTransformsAttr().Get(&bindTransforms);

		StableHashCombine64(hash, static_cast<std::uint64_t>(jointOrder.size()));
		for (size_t jointIndex = 0; jointIndex < jointOrder.size(); ++jointIndex) {
			StableHashCombine64(hash, StableHashString64(jointOrder[jointIndex].GetString()));
			StableHashCombine64(hash, static_cast<std::uint64_t>(static_cast<std::int64_t>(topology.GetParent(jointIndex))));
			if (jointIndex < bindTransforms.size()) {
				StableHashMatrix64(hash, bindTransforms[jointIndex]);
			}
			else {
				StableHashMatrix64(hash, GfMatrix4d(1.0));
			}
		}

		return hash;
	}

	static std::wstring AssetAssemblyBucketName(const AssetAssemblyBucketKey& key, std::size_t skinnedIndex, std::size_t rigidIndex)
	{
		if (!key.skinned) {
			return rigidIndex == 0u
				? L"__CLodAssetAssembly"
				: s2ws("__CLodAssetAssembly_Material" + std::to_string(rigidIndex));
		}
		return s2ws("__CLodAssetAssembly_Skinned" + std::to_string(skinnedIndex));
	}

	static bool AssetPathHasAnyPrefix(const SdfPath& path, const std::vector<SdfPath>& prefixes)
	{
		for (const SdfPath& prefix : prefixes) {
			if (path.HasPrefix(prefix)) {
				return true;
			}
		}
		return false;
	}

	std::optional<CLodCacheLoader::MeshCacheIdentity> BuildAssetAssemblyIdentity(
		const UsdStageRefPtr& stage,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode,
		const AssetAssemblyBucketInfo& bucket)
	{
		if (!stage) {
			return std::nullopt;
		}
		auto identity = USDGeometryExtractor::BuildWholeAssetAssemblyIdentity(stage, sourceIdentifier, geomTimeCode, bucket.firstMesh);
		if (!identity) {
			return std::nullopt;
		}
		USDGeometryExtractor::AppendWholeAssetAssemblyBucketIdentity(
			*identity,
			bucket.key.skinned,
			bucket.key.skinDomain,
			bucket.key.materialPath);
		return identity;
	}

	static std::optional<AssetAssemblyBucketKey> ClassifyAssetAssemblyMesh(
		const UsdGeomMesh& mesh,
		const UsdShadeMaterial& material,
		UsdSkelCache& skelCache,
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		std::unordered_map<AssetAssemblyBucketKey, std::shared_ptr<Skeleton>, AssetAssemblyBucketKeyHash>& skeletonsByKey,
		std::string* fallbackReason)
	{
		if (!mesh || IsBrNiflyCollisionMesh(mesh) || IsBrNiflyLODRenderMesh(mesh)) {
			return std::nullopt;
		}
		if (fallbackReason) {
			fallbackReason->clear();
		}

		AssetAssemblyBucketKey key{};
		key.materialPath = AssetAssemblyMaterialDomainPath(material);

		auto skinningQuery = USDGeometryExtractor::GetSkinningQuery(mesh, skelCache);
		if (!skinningQuery) {
			if (MeshHasUsdSkinningPrimvars(mesh)) {
				if (fallbackReason) {
					*fallbackReason = "skinned USD mesh '" + mesh.GetPrim().GetPath().GetString() +
						"' has joint primvars but no inherited UsdSkel skeleton";
				}
				return std::nullopt;
			}
			return key;
		}

		UsdSkelBindingAPI bindingAPI(mesh.GetPrim());
		UsdSkelSkeleton skel = bindingAPI.GetInheritedSkeleton();
		if (!skel) {
			if (fallbackReason) {
				*fallbackReason = "skinned USD mesh '" + mesh.GetPrim().GetPath().GetString() +
					"' produced a skinning query but no inherited UsdSkel skeleton";
			}
			return std::nullopt;
		}

		if (const UsdSkelRoot skelRoot = UsdSkelRoot::Find(skel.GetPrim()))
			skelCache.Populate(skelRoot, UsdPrimDefaultPredicate);
		UsdSkelSkeletonQuery skelQuery = skelCache.GetSkelQuery(skel);
		if (!skelQuery) {
			if (fallbackReason) {
				*fallbackReason = "UsdSkel skeleton query could not be built for '" +
					skel.GetPrim().GetPath().GetString() + "'";
			}
			return std::nullopt;
		}

		const VtTokenArray jointOrder = skelQuery.GetJointOrder();
		key.skinned = true;
		key.skinDomain = ComputeUsdSkeletonDomainHash(skel, jointOrder, skelQuery);

		if (!skeletonsByKey.contains(key)) {
			auto skeleton = BuildPayloadSkeleton(skel, jointOrder, skelQuery, metersPerUnit, true, stage);
			if (!skeleton) {
				if (fallbackReason) {
					*fallbackReason = "failed to build base skeleton for '" +
						skel.GetPrim().GetPath().GetString() + "'";
				}
				return std::nullopt;
			}
			skeletonsByKey.emplace(key, std::move(skeleton));
		}

		(void)stage;
		return key;
	}

	static void AttachAssemblySkeletonBucketsToPrimary(std::vector<AssetAssemblyBucketInfo>& buckets)
	{
		struct Candidate
		{
			size_t bucketIndex = 0;
			PayloadSkeletonBuildMetadata metadata;
		};

		std::vector<Candidate> candidates;
		candidates.reserve(buckets.size());
		for (size_t bucketIndex = 0; bucketIndex < buckets.size(); ++bucketIndex) {
			const auto& bucket = buckets[bucketIndex];
			if (!bucket.key.skinned || !bucket.skeleton) {
				continue;
			}
			const auto metadataIt = loadingCache.payloadSkeletonMetadata.find(bucket.skeleton.get());
			if (metadataIt == loadingCache.payloadSkeletonMetadata.end() ||
				metadataIt->second.bindXforms.empty() ||
				metadataIt->second.boneNames.empty()) {
				continue;
			}
			candidates.push_back(Candidate{ bucketIndex, metadataIt->second });
		}

		if (candidates.size() < 2u) {
			return;
		}

		auto primaryIt = std::max_element(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
			return lhs.metadata.boneNames.size() < rhs.metadata.boneNames.size();
		});
		if (primaryIt == candidates.end() || primaryIt->metadata.boneNames.size() < 2u) {
			return;
		}

		const PayloadSkeletonBuildMetadata& primary = primaryIt->metadata;
		std::vector<GfVec3d> primaryPositions;
		primaryPositions.reserve(primary.bindXforms.size());
		for (const GfMatrix4d& bind : primary.bindXforms) {
			primaryPositions.push_back(ExtractUsdMatrixTranslation(bind));
		}

		size_t attachedSkeletons = 0;
		size_t attachedRoots = 0;
		for (const Candidate& candidate : candidates) {
			AssetAssemblyBucketInfo& bucket = buckets[candidate.bucketIndex];
			const PayloadSkeletonBuildMetadata& metadata = candidate.metadata;
			if (&metadata == &primary || metadata.skeletonPath == primary.skeletonPath) {
				continue;
			}
			if (metadata.parentIndices.size() != metadata.boneNames.size() ||
				metadata.bindXforms.size() != metadata.boneNames.size()) {
				continue;
			}

			std::vector<DirectX::XMMATRIX> inverseBindMatrices;
			std::vector<Components::Transform> restLocalTransforms;
			std::vector<DirectX::XMMATRIX> rootParentGlobals(
				metadata.boneNames.size(),
				DirectX::XMMatrixIdentity());
			inverseBindMatrices.reserve(metadata.boneNames.size());
			restLocalTransforms.reserve(metadata.boneNames.size());
			const GfMatrix4d attachmentTransform = bucket.skeletonInstanceTransforms.empty()
				? GfMatrix4d(1.0)
				: bucket.skeletonInstanceTransforms.front();
			if (bucket.skeletonInstanceTransforms.size() > 1u) {
				spdlog::info(
					"USD assembly skeleton attach '{}': bucket has {} instance transforms; using first for external root parent in current per-bucket skin representation",
					metadata.skeletonPath,
					bucket.skeletonInstanceTransforms.size());
			}

			size_t skeletonRootAttachments = 0;
			for (size_t jointIndex = 0; jointIndex < metadata.boneNames.size(); ++jointIndex) {
				const int32_t parentIndex = metadata.parentIndices[jointIndex];
				const GfMatrix4d bindMatrix = metadata.bindXforms[jointIndex];
				const GfMatrix4d attachmentBindMatrix = bindMatrix * attachmentTransform;
				GfMatrix4d localBindMatrix = bindMatrix;

				if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < metadata.bindXforms.size()) {
					localBindMatrix = bindMatrix * metadata.bindXforms[static_cast<size_t>(parentIndex)].GetInverse();
				}
				else if (!primary.bindXforms.empty()) {
					const GfVec3d rootPosition = ExtractUsdMatrixTranslation(attachmentBindMatrix);
					size_t bestParent = 0;
					double bestDistance = std::numeric_limits<double>::max();
					for (size_t primaryJointIndex = 0; primaryJointIndex < primaryPositions.size(); ++primaryJointIndex) {
						const double distance = UsdDistance(rootPosition, primaryPositions[primaryJointIndex]);
						if (distance < bestDistance) {
							bestDistance = distance;
							bestParent = primaryJointIndex;
						}
					}

					const GfMatrix4d& parentBind = primary.bindXforms[bestParent];
					localBindMatrix = attachmentBindMatrix * parentBind.GetInverse();
					rootParentGlobals[jointIndex] = DirectXMatrixFromUsdMatrix(parentBind, metadata.metersPerUnit);
					++skeletonRootAttachments;
					++attachedRoots;

					spdlog::info(
						"USD assembly skeleton attach '{}': root '{}' -> primary '{}' joint '{}' distance={:.4f}",
						metadata.skeletonPath,
						UsdJointLeafName(metadata.boneNames[jointIndex]),
						primary.skeletonPath,
						UsdJointLeafName(primary.boneNames[bestParent]),
						bestDistance);
				}

				restLocalTransforms.push_back(ComponentsTransformFromUsdMatrix(localBindMatrix, metadata.metersPerUnit));
				inverseBindMatrices.push_back(DirectX::XMMatrixInverse(nullptr, DirectXMatrixFromUsdMatrix(bindMatrix, metadata.metersPerUnit)));
			}

			if (skeletonRootAttachments == 0u) {
				continue;
			}

			auto attachedSkeleton = std::make_shared<Skeleton>(
				metadata.boneNames,
				metadata.parentIndices,
				std::move(inverseBindMatrices),
				std::move(restLocalTransforms),
				std::move(rootParentGlobals),
				metadata.windSimulationGroupIndices,
				metadata.windProfileIdentity,
				metadata.dynamicWindMetadata);
			if (bucket.skeleton) {
				for (const auto& animation : bucket.skeleton->animations) {
					if (animation) {
						attachedSkeleton->AddAnimation(animation);
					}
				}
			}
			bucket.skeleton = attachedSkeleton;
			loadingCache.payloadSkeletonMetadata[attachedSkeleton.get()] = metadata;
			++attachedSkeletons;
		}

		if (attachedRoots != 0u) {
			spdlog::info(
				"USD assembly skeleton attachments: primary='{}' primaryJoints={} attachedSkeletons={} attachedRoots={}",
				primary.skeletonPath,
				primary.boneNames.size(),
				attachedSkeletons,
				attachedRoots);
		}
	}

	std::vector<AssetAssemblyBucketInfo> DiscoverAssetAssemblyBuckets(
		const UsdStageRefPtr& stage,
		UsdSkelCache& skelCache,
		double metersPerUnit,
		UsdTimeCode geomTimeCode,
		const InMemoryStageOptions& stageOptions,
		const ImportSettings& importSettings,
		std::string* fallbackReason)
	{
		ZoneScopedN("USDLoader::AssetAssembly::BucketizeParts");
		std::unordered_map<AssetAssemblyBucketKey, AssetAssemblyBucketInfo, AssetAssemblyBucketKeyHash> buckets;
		std::unordered_map<AssetAssemblyBucketKey, std::shared_ptr<Skeleton>, AssetAssemblyBucketKeyHash> skeletonsByKey;

		auto addMeshToBucket = [&](const UsdGeomMesh& mesh, const UsdShadeMaterial& material, const std::optional<UsdGeomSubset>& subset, const GfMatrix4d& transform) -> bool {
			if (!mesh) {
				return true;
			}
			if (ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(
				BuildGeometryExtractOptions(mesh, material, stageOptions))) {
				spdlog::debug(
					"USD whole-asset CLod discovery omitting temporary BRNifly overlay '{}'.",
					mesh.GetPrim().GetPath().GetString());
				return true;
			}
			std::string localReason;
			auto key = ClassifyAssetAssemblyMesh(mesh, material, skelCache, stage, metersPerUnit, skeletonsByKey, &localReason);
			if (!key) {
				if (!localReason.empty() && fallbackReason) {
					*fallbackReason = localReason;
				}
				return localReason.empty();
			}

			auto& bucket = buckets[*key];
			bucket.key = *key;
			if (!bucket.firstMesh) {
				bucket.firstMesh = mesh;
			}
			if (!bucket.material && material) {
				bucket.material = material;
			}
			if (bucket.staticTextureOverrideSourceName.empty()) {
				bucket.staticTextureOverrideSourceName = mesh.GetPrim().GetName().GetString();
			}
			if (key->skinned) {
				bucket.skeleton = skeletonsByKey[*key];
				if (bucket.skeletonInstanceTransforms.size() < 64u) {
					bucket.skeletonInstanceTransforms.push_back(transform);
				}
			}
			bool authoredDoubleSided = false;
			if (UsdGeomGprim gprim(mesh.GetPrim()); gprim) {
				gprim.GetDoubleSidedAttr().Get(&authoredDoubleSided, geomTimeCode);
			}
			bucket.forceDoubleSided =
				bucket.forceDoubleSided ||
				authoredDoubleSided ||
				ShouldForceDoubleSidedByName(material, subset, importSettings);
			bucket.meshPaths.push_back(mesh.GetPrim().GetPath().GetString());
			return true;
		};

		for (const USDGeometryExtractor::AssemblyMeshInstance& meshInstance :
			USDGeometryExtractor::EnumerateAssemblyMeshInstances(stage, geomTimeCode)) {
			UsdShadeMaterialBindingAPI bindAPI(meshInstance.mesh);
			auto subsets = bindAPI.GetMaterialBindSubsets();
			if (subsets.empty()) {
				if (!addMeshToBucket(
					meshInstance.mesh,
					bindAPI.ComputeBoundMaterial(),
					std::nullopt,
					meshInstance.localToStage)) {
					return {};
				}
			}
			else {
				for (const UsdGeomSubset& subset : subsets) {
					if (!addMeshToBucket(
						meshInstance.mesh,
						UsdShadeMaterialBindingAPI(subset).ComputeBoundMaterial(),
						subset,
						meshInstance.localToStage)) {
						return {};
					}
				}
			}
		}
		std::vector<AssetAssemblyBucketInfo> orderedBuckets;
		orderedBuckets.reserve(buckets.size());
		for (auto& [_, bucket] : buckets) {
			std::sort(bucket.meshPaths.begin(), bucket.meshPaths.end());
			bucket.meshPaths.erase(std::unique(bucket.meshPaths.begin(), bucket.meshPaths.end()), bucket.meshPaths.end());
			orderedBuckets.push_back(std::move(bucket));
		}
		std::sort(orderedBuckets.begin(), orderedBuckets.end(), [](const auto& lhs, const auto& rhs) {
			if (lhs.key.skinned != rhs.key.skinned) {
				return !lhs.key.skinned;
			}
			if (lhs.key.skinDomain != rhs.key.skinDomain) {
				return lhs.key.skinDomain < rhs.key.skinDomain;
			}
			return lhs.key.materialPath < rhs.key.materialPath;
		});
		return orderedBuckets;
	}

	std::shared_ptr<Mesh> BuildMeshFromAssetAssemblyPrebuilt(
		std::optional<ClusterLODPrebuiltData>&& prebuilt,
		const std::shared_ptr<Material>& material)
	{
		if (!prebuilt) {
			return nullptr;
		}
		std::string artifactError;
		std::shared_ptr<Skeleton> baseSkeleton = SkeletonArtifactCache::ResolveSkeleton(prebuilt->assemblySkeletonArtifact, &artifactError);
		if (!prebuilt->assemblySkeletonArtifact.Empty() && !baseSkeleton) {
			spdlog::warn("USD CLod skeleton artifact {} could not be resolved: {}",
				prebuilt->assemblySkeletonArtifact.id.ToString(), artifactError);
			return nullptr;
		}
		MeshIngestBuilder ingest(
			0u,
			0u,
			baseSkeleton ? VertexFlags::VERTEX_SKINNED : 0u,
			GetDefaultBuilderSettings());
		auto mesh = ingest.Build(material ? material : Material::GetDefaultMaterial(), std::move(prebuilt), MeshCpuDataPolicy::ReleaseAfterUpload);
		if (mesh && baseSkeleton) {
			mesh->SetBaseSkin(baseSkeleton);
		}
		return mesh;
	}

	bool TryLoadAssetAssemblyMeshes(
		const UsdStageRefPtr& stage,
		const StageImportContext& stageContext,
		const ImportSettings& importSettings,
		const InMemoryStageOptions& options,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode,
		const std::vector<AssetAssemblyBucketInfo>& buckets,
		std::vector<std::shared_ptr<Mesh>>& meshes)
	{
		ZoneScopedN("USDLoader::LoadModelFromStage::TryLoadAssetAssemblyCache");
		meshes.clear();
		meshes.reserve(buckets.size());
		for (const AssetAssemblyBucketInfo& bucket : buckets) {
			auto identity = BuildAssetAssemblyIdentity(stage, sourceIdentifier, geomTimeCode, bucket);
			if (!identity) {
				return false;
			}
			auto prebuilt = CLodCacheLoader::TryLoadPrebuilt(*identity);
			if (!prebuilt || prebuilt->groups.empty() || prebuilt->assemblyInstances.empty()) {
				meshes.clear();
				return false;
			}
			if (bucket.material) {
				ProcessMaterial(
					bucket.material,
					stage,
					options,
					stageContext.isUSDZ,
					stageContext.directory,
					importSettings.loadMaterialTextures);
			}
			const std::vector<MeshUvSetData> materialUvSets = BuildMaterialUvSetDescriptors(bucket.material);
			auto material = ResolveMaterialForMesh(
				bucket.material,
				materialUvSets,
				bucket.forceDoubleSided,
				bucket.firstMesh.GetPrim(),
				nullptr,
				bucket.staticTextureOverrideSourceName);
			auto mesh = BuildMeshFromAssetAssemblyPrebuilt(std::move(prebuilt), material);
			if (!mesh) {
				meshes.clear();
				return false;
			}
			meshes.push_back(std::move(mesh));
		}
		return !meshes.empty();
	}

	bool BuildAssetAssemblyMeshesFromPreprocessedData(
		const UsdStageRefPtr& stage,
		const StageImportContext& stageContext,
		const ImportSettings& importSettings,
		const InMemoryStageOptions& options,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode,
		const std::vector<AssetAssemblyBucketInfo>& buckets,
		std::vector<std::shared_ptr<Mesh>>& meshes,
		std::string* outFailureReason)
	{
		ZoneScopedN("USDLoader::LoadModelFromStage::BuildAssetAssemblyCache");
		meshes.clear();
		meshes.reserve(buckets.size());
		if (outFailureReason) outFailureReason->clear();
		auto fail = [&](std::string reason) {
			if (outFailureReason) *outFailureReason = std::move(reason);
			meshes.clear();
			return false;
		};

		struct BuildBucket {
			const AssetAssemblyBucketInfo* info = nullptr;
			std::vector<ClusterLODAssemblyPart> parts;
			std::vector<ClusterLODAssemblyInstanceSpec> instances;
			std::unordered_map<const MeshPreprocessResult*, uint32_t> partByResult;
			std::vector<MeshUvSetData> representativeUvSets;
			std::string staticTextureOverrideSourceName;
			std::vector<GfMatrix4d> instanceTransforms;
			std::vector<std::string> instanceBindJoints;
			bool forceDoubleSided = false;
		};

		std::unordered_map<AssetAssemblyBucketKey, std::size_t, AssetAssemblyBucketKeyHash> bucketIndexByKey;
		std::vector<BuildBucket> buildBuckets;
		buildBuckets.reserve(buckets.size());
		for (const AssetAssemblyBucketInfo& bucket : buckets) {
			bucketIndexByKey.emplace(bucket.key, buildBuckets.size());
			buildBuckets.push_back(BuildBucket{ .info = &bucket });
		}

		auto addResultInstance = [&](BuildBucket& bucket, const MeshPreprocessResult& result, const GfMatrix4d& transform, std::string_view bindJoint) -> bool {
			if (!result.transientArtifacts) {
				return fail("missing retained CLod artifacts for '" + result.sourcePrimPath + "'");
			}
			uint32_t partIndex = 0u;
			const auto existing = bucket.partByResult.find(&result);
			if (existing != bucket.partByResult.end()) {
				partIndex = existing->second;
			}
			else {
				partIndex = static_cast<uint32_t>(bucket.parts.size());
				bucket.parts.push_back(ClusterLODAssemblyPart{
					.artifacts = result.transientArtifacts.get(),
					.coverageVertices = &result.ingest.GetVertices(),
					.coverageIndices = &result.ingest.GetIndices(),
					.coverageVertexSize = result.ingest.GetVertexSize(),
					.coverageSkinningVertices = &result.ingest.GetSkinningVertices(),
					.coverageSkinningVertexSize = result.ingest.GetSkinningVertexSize(),
					.doubleSidedCoverageTriangles = result.forceDoubleSidedPreview });
				bucket.partByResult.emplace(&result, partIndex);
			}
			bucket.instances.push_back(ClusterLODAssemblyInstanceSpec{
				.partIndex = partIndex,
				.rootNode = 0u,
				.transform = USDGeometryExtractor::AssemblyTransformFromUsdMatrix(transform, stageContext.metersPerUnit),
				.flags = 0u,
			});
			bucket.instanceTransforms.push_back(transform);
			bucket.instanceBindJoints.emplace_back(bindJoint);
			return true;
		};

		auto addMeshInstances = [&](const UsdGeomMesh& mesh, const GfMatrix4d& transform, std::string_view bindJoint) -> bool {
			std::string ignoredReason;
			UsdSkelCache localSkelCache;
			std::unordered_map<AssetAssemblyBucketKey, std::shared_ptr<Skeleton>, AssetAssemblyBucketKeyHash> ignoredSkeletons;
			const auto recordIt = loadingCache.preprocessedMeshCache.find(mesh.GetPrim().GetPath().GetString());
			if (recordIt == loadingCache.preprocessedMeshCache.end()) {
				const std::string meshPath = mesh.GetPrim().GetPath().GetString();
				if (const auto skipped = loadingCache.skippedPreprocessedMeshReasons.find(meshPath);
					skipped != loadingCache.skippedPreprocessedMeshReasons.end()) {
					spdlog::debug(
						"USD whole-asset CLod assembly omitting intentionally skipped mesh '{}': {}.",
						meshPath,
						skipped->second);
					return true;
				}
				return fail("unexpectedly missing preprocessed mesh '" + meshPath + "'");
			}
			for (const PreprocessedMeshSubset& subset : recordIt->second.subsets) {
				auto key = ClassifyAssetAssemblyMesh(mesh, subset.material, localSkelCache, stage, UsdGeomGetStageMetersPerUnit(stage), ignoredSkeletons, &ignoredReason);
				if (!key) {
					if (ignoredReason.empty()) return true;
					return fail("mesh '" + mesh.GetPrim().GetPath().GetString() + "' is not assembly-compatible: " + ignoredReason);
				}
				const auto bucketIt = bucketIndexByKey.find(*key);
				if (bucketIt == bucketIndexByKey.end()) {
					continue;
				}
				BuildBucket& bucket = buildBuckets[bucketIt->second];
				bucket.forceDoubleSided =
					bucket.forceDoubleSided ||
					recordIt->second.authoredDoubleSided ||
					subset.inferredDoubleSided ||
					subset.result.forceDoubleSidedPreview;
				if (bucket.representativeUvSets.empty()) {
					bucket.representativeUvSets = subset.result.ingest.GetUvSets();
				}
				if (bucket.staticTextureOverrideSourceName.empty()) {
					bucket.staticTextureOverrideSourceName = subset.staticTextureOverrideSourceName.empty()
						? mesh.GetPrim().GetName().GetString()
						: subset.staticTextureOverrideSourceName;
				}
				if (!addResultInstance(bucket, subset.result, transform, bindJoint)) {
					return false;
				}
			}
			return true;
		};

		for (const USDGeometryExtractor::AssemblyMeshInstance& meshInstance :
			USDGeometryExtractor::EnumerateAssemblyMeshInstances(stage, geomTimeCode)) {
			const GfMatrix4d correctedTransform =
				meshInstance.localToStage * GfMatrix4d(stageContext.upRot, GfVec3d(0.0));
			if (!addMeshInstances(meshInstance.mesh, correctedTransform, meshInstance.assemblyBindJoint)) {
				return fail(outFailureReason && !outFailureReason->empty()
					? *outFailureReason
					: "failed to add an enumerated mesh instance");
			}
		}

		ClusterLODAssemblySkeletonData expandedAssemblySkeleton;
		std::vector<GfMatrix4d> expandedBindGlobals;
		std::unordered_map<std::uint64_t, std::vector<uint32_t>> remapByInstanceKey;
		std::unordered_map<std::string, uint32_t> expandedJointByAuthoredName;
		UsdGeomXformCache skeletonXformCache(geomTimeCode);
		auto skeletonInstanceTransform = [&](const PayloadSkeletonBuildMetadata& metadata, const GfMatrix4d& meshInstanceTransform) {
			const UsdPrim skeletonPrim = stage->GetPrimAtPath(SdfPath(metadata.skeletonPath));
			const UsdSkelRoot skeletonRoot = skeletonPrim ? UsdSkelRoot::Find(skeletonPrim) : UsdSkelRoot();
			const UsdPrim defaultPrim = stage->GetDefaultPrim();
			if (skeletonRoot && defaultPrim && skeletonRoot.GetPrim() == defaultPrim) {
				// A skeleton authored on the assembly root is already a complete, assembly-space
				// hierarchy. Every part inherits it, but that does not make it a part-local
				// skeleton that should be transformed and duplicated for every mesh instance.
				return skeletonXformCache.GetLocalToWorldTransform(skeletonRoot.GetPrim()) *
					GfMatrix4d(stageContext.upRot, GfVec3d(0.0));
			}
			return meshInstanceTransform;
		};
		auto buildInstanceKey = [](const PayloadSkeletonBuildMetadata& metadata, const GfMatrix4d& transform, std::string_view bindJoint) {
			std::uint64_t hash = StableHashString64(metadata.skeletonPath);
			for (const std::string& name : metadata.boneNames) {
				StableHashCombine64(hash, StableHashString64(name));
			}
			StableHashMatrix64(hash, transform);
			StableHashCombine64(hash, StableHashString64(std::string(bindJoint)));
			return hash;
		};
		auto appendOrReuseSkeletonInstance = [&](const std::shared_ptr<Skeleton>& sourceSkeleton, const GfMatrix4d& transform, std::string_view bindJoint) -> std::vector<uint32_t> {
			if (!sourceSkeleton) {
				return {};
			}
			const auto metadataIt = loadingCache.payloadSkeletonMetadata.find(sourceSkeleton.get());
			if (metadataIt == loadingCache.payloadSkeletonMetadata.end()) {
				return {};
			}
			const PayloadSkeletonBuildMetadata& metadata = metadataIt->second;
			const std::uint64_t instanceKey = buildInstanceKey(metadata, transform, bindJoint);
			if (auto existing = remapByInstanceKey.find(instanceKey); existing != remapByInstanceKey.end()) {
				return existing->second;
			}

			std::vector<uint32_t> remap(metadata.boneNames.size(), 0u);
			const uint32_t baseJoint = static_cast<uint32_t>(expandedAssemblySkeleton.jointNames.size());
			if (baseJoint == 0u) {
				expandedAssemblySkeleton.windProfileIdentity = metadata.windProfileIdentity;
				expandedAssemblySkeleton.dynamicWindMetadata = metadata.dynamicWindMetadata;
				expandedAssemblySkeleton.dynamicWindMetadata.bones.clear();
			}
			std::vector<uint32_t> assemblyGroupByLocal(metadata.dynamicWindMetadata.groups.size(), 0xFFFFFFFFu);
			for (uint32_t localGroup = 0; localGroup < assemblyGroupByLocal.size(); ++localGroup) {
				const auto& sourceGroup = metadata.dynamicWindMetadata.groups[localGroup];
				auto found = std::ranges::find_if(expandedAssemblySkeleton.dynamicWindMetadata.groups, [&](const auto& candidate) {
					return candidate.role == sourceGroup.role && candidate.profileGroupId == sourceGroup.profileGroupId &&
						candidate.flags == sourceGroup.flags && candidate.reductionPriority == sourceGroup.reductionPriority &&
						candidate.minimumDriverCount == sourceGroup.minimumDriverCount;
				});
				if (found == expandedAssemblySkeleton.dynamicWindMetadata.groups.end()) {
					assemblyGroupByLocal[localGroup] = static_cast<uint32_t>(expandedAssemblySkeleton.dynamicWindMetadata.groups.size());
					expandedAssemblySkeleton.dynamicWindMetadata.groups.push_back(sourceGroup);
				}
				else assemblyGroupByLocal[localGroup] = static_cast<uint32_t>(std::distance(expandedAssemblySkeleton.dynamicWindMetadata.groups.begin(), found));
			}
			auto attachedGroup = [&]() {
				auto found = std::ranges::find(expandedAssemblySkeleton.dynamicWindMetadata.groups,
					DynamicWindSimulationGroupRole::AttachedBranch, &DynamicWindSimulationGroupData::role);
				if (found != expandedAssemblySkeleton.dynamicWindMetadata.groups.end())
					return static_cast<uint32_t>(std::distance(expandedAssemblySkeleton.dynamicWindMetadata.groups.begin(), found));
				DynamicWindSimulationGroupData group;
				if (expandedAssemblySkeleton.dynamicWindMetadata.groups.size() > 1u)
					group = expandedAssemblySkeleton.dynamicWindMetadata.groups[1u];
				group.flags &= ~DynamicWindMetadata::GroupFlagTrunk;
				group.role = DynamicWindSimulationGroupRole::AttachedBranch;
				group.profileGroupId = expandedAssemblySkeleton.dynamicWindMetadata.attachedBranchProfileGroupId;
				group.reductionPriority = 1.0f;
				group.minimumDriverCount = 0u;
				const uint32_t index = static_cast<uint32_t>(expandedAssemblySkeleton.dynamicWindMetadata.groups.size());
				expandedAssemblySkeleton.dynamicWindMetadata.groups.push_back(group);
				return index;
			};
			std::vector<uint32_t> generatedAttachedLocals;
			for (uint32_t jointIndex = 0; jointIndex < static_cast<uint32_t>(metadata.boneNames.size()); ++jointIndex) {
				const GfMatrix4d sourceBind = jointIndex < metadata.bindXforms.size()
					? metadata.bindXforms[jointIndex]
					: GfMatrix4d(1.0);
				const GfMatrix4d expandedBind = sourceBind * transform;
				int32_t parentIndex = jointIndex < metadata.parentIndices.size()
					? metadata.parentIndices[jointIndex]
					: -1;
				if (parentIndex >= 0 && static_cast<uint32_t>(parentIndex) < jointIndex) {
					parentIndex = static_cast<int32_t>(baseJoint + static_cast<uint32_t>(parentIndex));
				}
				else {
					parentIndex = -1;
					if (!bindJoint.empty()) {
						if (const auto authoredParent = expandedJointByAuthoredName.find(std::string(bindJoint));
							authoredParent != expandedJointByAuthoredName.end()) {
							parentIndex = static_cast<int32_t>(authoredParent->second);
						}
						else {
							spdlog::warn(
								"USD assembly skeleton instance '{}' references missing bind joint '{}'; leaving root detached.",
								metadata.skeletonPath,
								bindJoint);
						}
					}
					else if (!expandedBindGlobals.empty()) {
						const GfVec3d rootTranslation = expandedBind.ExtractTranslation();
						double bestDistanceSquared = std::numeric_limits<double>::infinity();
						int32_t bestParent = -1;
						for (uint32_t candidateIndex = 0; candidateIndex < static_cast<uint32_t>(expandedBindGlobals.size()); ++candidateIndex) {
							const GfVec3d delta = rootTranslation - expandedBindGlobals[candidateIndex].ExtractTranslation();
							const double distanceSquared = GfDot(delta, delta);
							if (distanceSquared < bestDistanceSquared) {
								bestDistanceSquared = distanceSquared;
								bestParent = static_cast<int32_t>(candidateIndex);
							}
						}
						parentIndex = bestParent;
					}
				}

				GfMatrix4d restLocal = expandedBind;
				if (parentIndex >= 0 && static_cast<uint32_t>(parentIndex) < expandedBindGlobals.size()) {
					restLocal = expandedBind * expandedBindGlobals[static_cast<uint32_t>(parentIndex)].GetInverse();
				}

				expandedAssemblySkeleton.jointNames.push_back(
					metadata.skeletonPath + "[" + std::to_string(remapByInstanceKey.size()) + "]/" + metadata.boneNames[jointIndex]);
				expandedJointByAuthoredName.try_emplace(metadata.boneNames[jointIndex], baseJoint + jointIndex);
				expandedJointByAuthoredName.try_emplace(
					std::string(UsdJointLeafName(metadata.boneNames[jointIndex])),
					baseJoint + jointIndex);
				expandedAssemblySkeleton.parentIndices.push_back(parentIndex);
				DirectX::XMFLOAT4X4 inverseBind{};
				DirectX::XMFLOAT4X4 restLocalMatrix{};
				DirectX::XMFLOAT4X4 bindGlobalMatrix{};
				StoreMatrix4x4(inverseBind, DirectX::XMMatrixInverse(nullptr, DirectXMatrixFromUsdMatrix(expandedBind, metadata.metersPerUnit)));
				StoreMatrix4x4(restLocalMatrix, DirectXMatrixFromUsdMatrix(restLocal, metadata.metersPerUnit));
				StoreMatrix4x4(bindGlobalMatrix, DirectXMatrixFromUsdMatrix(expandedBind, metadata.metersPerUnit));
				expandedAssemblySkeleton.inverseBindMatrices.push_back(inverseBind);
				expandedAssemblySkeleton.restLocalMatrices.push_back(restLocalMatrix);
				expandedAssemblySkeleton.bindGlobalMatrices.push_back(bindGlobalMatrix);
				const uint32_t localGroup = jointIndex < metadata.windSimulationGroupIndices.size()
					? metadata.windSimulationGroupIndices[jointIndex]
					: 0xFFFFFFFFu;
				uint32_t assemblyGroup = localGroup < assemblyGroupByLocal.size() ? assemblyGroupByLocal[localGroup] : 0xFFFFFFFFu;
				if (assemblyGroup == 0xFFFFFFFFu && !bindJoint.empty()) {
					assemblyGroup = attachedGroup();
					generatedAttachedLocals.push_back(jointIndex);
				}
				expandedAssemblySkeleton.windSimulationGroupIndices.push_back(assemblyGroup);
				DynamicWindBoneData windBone;
				if (jointIndex < metadata.dynamicWindMetadata.bones.size()) {
					windBone = metadata.dynamicWindMetadata.bones[jointIndex];
					if (windBone.chainOriginBoneIndex != 0xFFFFFFFFu &&
						windBone.chainOriginBoneIndex < metadata.boneNames.size()) {
						windBone.chainOriginBoneIndex += baseJoint;
					}
				}
				expandedAssemblySkeleton.dynamicWindMetadata.bones.push_back(windBone);
				expandedBindGlobals.push_back(expandedBind);
				remap[jointIndex] = baseJoint + jointIndex;
			}
			for (uint32_t localJoint : generatedAttachedLocals) {
				uint32_t origin = localJoint;
				uint32_t chainIndex = 0u;
				float distanceFromOrigin = 0.0f;
				while (origin < metadata.parentIndices.size() && metadata.parentIndices[origin] >= 0) {
					const uint32_t parent = static_cast<uint32_t>(metadata.parentIndices[origin]);
					if (!std::ranges::contains(generatedAttachedLocals, parent)) break;
					const auto& childBind = expandedAssemblySkeleton.bindGlobalMatrices[baseJoint + origin];
					const auto& parentBind = expandedAssemblySkeleton.bindGlobalMatrices[baseJoint + parent];
					const float dx = childBind._41 - parentBind._41;
					const float dy = childBind._42 - parentBind._42;
					const float dz = childBind._43 - parentBind._43;
					distanceFromOrigin += std::sqrt(dx * dx + dy * dy + dz * dz);
					origin = parent;
					++chainIndex;
				}
				auto& windBone = expandedAssemblySkeleton.dynamicWindMetadata.bones[baseJoint + localJoint];
				windBone.chainOriginBoneIndex = baseJoint + origin;
				windBone.indexInBoneChain = chainIndex;
				windBone.chainBoneCount = 1u;
				windBone.chainLength = distanceFromOrigin;
			}
			std::unordered_map<uint32_t, float> generatedChainLengths;
			for (uint32_t localJoint : generatedAttachedLocals) {
				auto& bone = expandedAssemblySkeleton.dynamicWindMetadata.bones[baseJoint + localJoint];
				if (bone.chainOriginBoneIndex >= expandedAssemblySkeleton.dynamicWindMetadata.bones.size()) continue;
				auto& origin = expandedAssemblySkeleton.dynamicWindMetadata.bones[bone.chainOriginBoneIndex];
				origin.chainBoneCount = (std::max)(origin.chainBoneCount, bone.indexInBoneChain + 1u);
				generatedChainLengths[bone.chainOriginBoneIndex] = (std::max)(generatedChainLengths[bone.chainOriginBoneIndex], bone.chainLength);
			}
			for (uint32_t localJoint : generatedAttachedLocals) {
				auto& bone = expandedAssemblySkeleton.dynamicWindMetadata.bones[baseJoint + localJoint];
				const auto& origin = expandedAssemblySkeleton.dynamicWindMetadata.bones[bone.chainOriginBoneIndex];
				bone.chainBoneCount = origin.chainBoneCount;
				bone.chainLength = generatedChainLengths[bone.chainOriginBoneIndex];
			}
			remapByInstanceKey.emplace(instanceKey, remap);
			return remap;
		};

		struct SkeletonAppendWorkItem
		{
			BuildBucket* bucket = nullptr;
			std::size_t instanceIndex = 0;
			std::size_t jointCount = 0;
			std::string skeletonPath;
		};

		std::vector<SkeletonAppendWorkItem> skeletonAppendWork;
		for (BuildBucket& bucket : buildBuckets) {
			if (bucket.info == nullptr || !bucket.info->key.skinned || !bucket.info->skeleton) {
				continue;
			}
			const auto metadataIt = loadingCache.payloadSkeletonMetadata.find(bucket.info->skeleton.get());
			if (metadataIt == loadingCache.payloadSkeletonMetadata.end()) {
				continue;
			}
			const PayloadSkeletonBuildMetadata& metadata = metadataIt->second;
			const std::size_t instanceCount = std::min(bucket.instances.size(), bucket.instanceTransforms.size());
			for (std::size_t instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex) {
				skeletonAppendWork.push_back(SkeletonAppendWorkItem{
					.bucket = &bucket,
					.instanceIndex = instanceIndex,
					.jointCount = metadata.boneNames.size(),
					.skeletonPath = metadata.skeletonPath,
				});
			}
		}
		std::sort(skeletonAppendWork.begin(), skeletonAppendWork.end(), [](const SkeletonAppendWorkItem& lhs, const SkeletonAppendWorkItem& rhs) {
			if (lhs.jointCount != rhs.jointCount) {
				return lhs.jointCount > rhs.jointCount;
			}
			if (lhs.skeletonPath != rhs.skeletonPath) {
				return lhs.skeletonPath < rhs.skeletonPath;
			}
			return lhs.instanceIndex < rhs.instanceIndex;
		});
		for (SkeletonAppendWorkItem& workItem : skeletonAppendWork) {
			if (workItem.bucket == nullptr || workItem.instanceIndex >= workItem.bucket->instances.size() ||
				workItem.instanceIndex >= workItem.bucket->instanceTransforms.size()) {
				continue;
			}
			workItem.bucket->instances[workItem.instanceIndex].boneRemapIndices =
				appendOrReuseSkeletonInstance(
					workItem.bucket->info->skeleton,
					skeletonInstanceTransform(
						loadingCache.payloadSkeletonMetadata.at(workItem.bucket->info->skeleton.get()),
						workItem.bucket->instanceTransforms[workItem.instanceIndex]),
					workItem.instanceIndex < workItem.bucket->instanceBindJoints.size()
						? std::string_view(workItem.bucket->instanceBindJoints[workItem.instanceIndex])
						: std::string_view{});
		}

		if (!expandedAssemblySkeleton.Empty()) {
			spdlog::info(
				"USD CLod assembly expanded skeleton: joints={} uniqueInstances={} buckets={} appendJobs={}",
				expandedAssemblySkeleton.jointNames.size(),
				remapByInstanceKey.size(),
				buildBuckets.size(),
				skeletonAppendWork.size());
			LogAssemblySkeletonTopologyDiagnostics(expandedAssemblySkeleton, "build");
		}
		SkeletonArtifactReference expandedSkeletonArtifact;
		if (!expandedAssemblySkeleton.Empty()) {
			std::string artifactError;
			auto savedArtifact = SkeletonArtifactCache::Save(expandedAssemblySkeleton, &artifactError);
			if (!savedArtifact) {
				spdlog::error("USD CLod assembly skeleton artifact save failed: {}", artifactError);
				return fail("skeleton artifact save failed: " + artifactError);
			}
			expandedSkeletonArtifact = *savedArtifact;
		}

		for (BuildBucket& bucket : buildBuckets) {
			if (bucket.parts.empty() || bucket.instances.empty()) {
				spdlog::warn(
					"USD whole-asset CLod assembly bucket produced no instances: skinned={}, domain={}.",
					bucket.info && bucket.info->key.skinned,
					bucket.info ? std::to_string(bucket.info->key.skinDomain) : std::string("<none>"));
				return fail("material bucket produced no parts or instances");
			}
			try {
				ClusterLODBuilderSettings assemblySettings = GetDefaultBuilderSettings(sourceIdentifier);
				assemblySettings.doubleSidedVoxelSourceNormals =
					bucket.forceDoubleSided ||
					(bucket.info != nullptr && bucket.info->forceDoubleSided);
				ClusterLODPrebuildArtifacts assemblyArtifacts =
					BuildClusterLODAssemblyArtifactsPreservingTriangleOnly(
						bucket.parts,
						bucket.instances,
						assemblySettings,
						8u);

				if (bucket.info != nullptr && bucket.info->key.skinned && !expandedSkeletonArtifact.Empty()) {
					assemblyArtifacts.prebuiltData.assemblySkeletonArtifact = expandedSkeletonArtifact;
				}

				auto identity = BuildAssetAssemblyIdentity(stage, sourceIdentifier, geomTimeCode, *bucket.info);
				if (!identity) {
					return fail("could not derive the material-bucket cache identity");
				}
				ClusterLODPrebuiltData savedPrebuiltData;
				if (!CLodCacheLoader::SavePrebuiltLocked(
					*identity,
					assemblyArtifacts.prebuiltData,
					assemblyArtifacts.cacheBuildData.AsPayload(),
					&savedPrebuiltData)) {
					return fail("material-bucket cache save failed");
				}
				auto prebuiltData = CLodCacheLoader::TryLoadPrebuilt(*identity);
				if (!prebuiltData || prebuiltData->assemblyInstances.empty()) {
					return fail("material-bucket cache could not be reopened after save or contained no assembly instances");
				}

				std::shared_ptr<Material> material = Material::GetDefaultMaterial();
				if (bucket.info && bucket.info->material) {
					ProcessMaterial(
						bucket.info->material,
						stage,
						options,
						stageContext.isUSDZ,
						stageContext.directory,
						importSettings.loadMaterialTextures);
					const std::vector<MeshUvSetData> materialUvSets = bucket.representativeUvSets.empty()
						? BuildMaterialUvSetDescriptors(bucket.info->material)
						: bucket.representativeUvSets;
					material = ResolveMaterialForMesh(
						bucket.info->material,
						materialUvSets,
						bucket.forceDoubleSided || bucket.info->forceDoubleSided,
						bucket.info->firstMesh.GetPrim(),
						nullptr,
						bucket.staticTextureOverrideSourceName.empty()
							? bucket.info->staticTextureOverrideSourceName
							: bucket.staticTextureOverrideSourceName);
				}
				auto mesh = BuildMeshFromAssetAssemblyPrebuilt(std::move(prebuiltData), material);
				if (!mesh) return fail("published material-bucket cache could not create a mesh");
				meshes.push_back(std::move(mesh));
			}
			catch (const std::exception& e) {
				spdlog::warn("USD whole-asset CLod assembly build failed: {}", e.what());
				return fail(std::string("assembly builder exception: ") + e.what());
			}
		}

		return !meshes.empty();
	}

	std::shared_ptr<Scene> CreateCollapsedAssetAssemblyScene(
		const std::vector<std::shared_ptr<Mesh>>& meshes,
		const std::vector<AssetAssemblyBucketInfo>& buckets,
		const GfRotation& upAxisCorrection)
	{
		ZoneScopedN("USDLoader::LoadModelFromStage::CreateCollapsedAssemblyScene");
		auto scene = std::make_shared<Scene>();
		std::vector<std::shared_ptr<Mesh>> entityMeshes;
		entityMeshes.reserve(meshes.size());
		for (const auto& mesh : meshes) {
			if (mesh) {
				entityMeshes.push_back(mesh);
			}
		}
		if (!entityMeshes.empty()) {
			flecs::entity entity = scene->CreateRenderableEntityECS(std::move(entityMeshes), L"__CLodAssetAssembly");
			(void)upAxisCorrection;
			if (auto* meshInstances = entity.try_get_mut<Components::MeshInstances>()) {
				std::unordered_map<const Skeleton*, std::shared_ptr<Skeleton>> runtimeSkeletonByBase;
				for (const auto& meshInstance : meshInstances->meshInstances) {
					if (!meshInstance || !meshInstance->HasSkin()) {
						continue;
					}
					auto runtimeSkeleton = meshInstance->GetSkin();
					auto baseSkeleton = runtimeSkeleton ? runtimeSkeleton->GetBaseSkeletonShared() : nullptr;
					if (!baseSkeleton) {
						continue;
					}
					auto& sharedRuntimeSkeleton = runtimeSkeletonByBase[baseSkeleton.get()];
					if (!sharedRuntimeSkeleton) {
						sharedRuntimeSkeleton = runtimeSkeleton;
						continue;
					}
					meshInstance->SetSkeleton(sharedRuntimeSkeleton);
					meshInstance->SyncSkinningStateFromSkeleton();
				}
				if (!runtimeSkeletonByBase.empty()) {
					meshInstances->BumpGeneration();
				}
			}
		}
		return scene;
	}

}
