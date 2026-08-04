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

#include "Materials/Material.h"
#include "Materials/MaterialFlags.h"
#include "Render/PSOFlags.h"
#include "Resources/Texture.h"
#include "Resources/Sampler.h"
#include "Import/Filetypes.h"
#include "Scene/Scene.h"
#include "Mesh/Mesh.h"
#include "Mesh/MeshInstance.h"
#include "Mesh/ClusterLODUtilities.h"
#include "Animation/Skeleton.h"
#include "Import/SkeletonArtifactCache.h"
#include "Scene/Components.h"
#include "Animation/AnimationController.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/TextureProcessingManager.h"

#include "Import/USDLoader.h"
#include "Import/USDMaterialCache.h"
#include "Import/CLodCacheLoader.h"
#include "Import/ObjectReyesAtlasBaker.h"
#include "Import/USDGeometryExtractor.h"
#include "Mesh/DefaultCLodSettings.h"
#include "Mesh/VertexLayout.h"
#include "Mesh/VertexLayout.h"

namespace USDLoader {

	std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin)
	{
		return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - begin).count());
	}

	using namespace pxr;
	using json = nlohmann::json;

	namespace {

		std::string NormalizeHeuristicName(const std::string& value) {
			std::string normalized;
			normalized.reserve(value.size());
			for (unsigned char ch : value) {
				if (std::isalnum(ch)) {
					normalized.push_back(static_cast<char>(std::tolower(ch)));
				}
			}
			return normalized;
		}

		std::vector<std::string> TokenizeHeuristicName(const std::string& value) {
			std::vector<std::string> tokens;
			std::string current;
			for (unsigned char ch : value) {
				if (std::isalnum(ch)) {
					current.push_back(static_cast<char>(std::tolower(ch)));
				}
				else if (!current.empty()) {
					tokens.push_back(std::move(current));
					current.clear();
				}
			}
			if (!current.empty()) {
				tokens.push_back(std::move(current));
			}
			return tokens;
		}

		bool NameSuggestsDoubleSided(const std::string& value) {
			const std::string normalized = NormalizeHeuristicName(value);
			if (normalized.find("doublesided") != std::string::npos ||
				normalized.find("doubleside") != std::string::npos ||
				normalized.find("twosided") != std::string::npos ||
				normalized.find("twoside") != std::string::npos ||
				normalized.find("2sided") != std::string::npos ||
				normalized.find("2side") != std::string::npos) {
				return true;
			}

			const std::vector<std::string> tokens = TokenizeHeuristicName(value);
			for (size_t tokenIndex = 0; tokenIndex + 1 < tokens.size(); ++tokenIndex) {
				const std::string& first = tokens[tokenIndex];
				const std::string& second = tokens[tokenIndex + 1];
				const bool firstMatches = first == "double" || first == "two" || first == "2";
				const bool secondMatches = second == "side" || second == "sided";
				if (firstMatches && secondMatches) {
					return true;
				}
			}

			return false;
		}

		bool ShouldForceDoubleSidedByName(
			const UsdShadeMaterial& material,
			const std::optional<UsdGeomSubset>& subset,
			const ImportSettings& settings) {
			if (!settings.enableDoubleSidedNameHeuristic) {
				return false;
			}

			if (subset && NameSuggestsDoubleSided(subset->GetPrim().GetName().GetString())) {
				return true;
			}

			if (material && NameSuggestsDoubleSided(material.GetPrim().GetName().GetString())) {
				return true;
			}

			return false;
		}

	}

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

	static UsdTimeCode GetUsdGeometrySampleTime(const UsdStageRefPtr& stage) {
		if (stage && stage->HasAuthoredTimeCodeRange()) {
			return UsdTimeCode(stage->GetStartTimeCode());
		}

		return UsdTimeCode::Default();
	}

	struct StageImportContext {
		double metersPerUnit = 1.0;
		GfRotation upRot;
		std::string directory;
		bool isUSDZ = false;
	};

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

	static void SetEntityTransformFromUsdMatrix(
		flecs::entity entity,
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		entity.set<Components::Position>({
			DirectX::XMFLOAT3(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit))
			});
		entity.set<Components::Rotation>({
			DirectX::XMFLOAT4(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))
			});
		entity.set<Components::Scale>({
			DirectX::XMFLOAT3(
				static_cast<float>(scale[0]),
				static_cast<float>(scale[1]),
				static_cast<float>(scale[2]))
		});
	}

	static DirectX::XMMATRIX DirectXMatrixFromUsdMatrix(
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		return DirectX::XMMatrixScaling(
			static_cast<float>(scale[0]),
			static_cast<float>(scale[1]),
			static_cast<float>(scale[2])) *
			DirectX::XMMatrixRotationQuaternion(DirectX::XMVectorSet(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))) *
			DirectX::XMMatrixTranslation(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit));
	}

	static Components::Transform ComponentsTransformFromUsdMatrix(
		const GfMatrix4d& matrix,
		double metersPerUnit)
	{
		const GfTransform transform(matrix);
		const GfVec3d translation = transform.GetTranslation();
		const GfQuaternion rotation = transform.GetRotation().GetQuaternion();
		const GfVec3d scale = transform.GetScale();

		return Components::Transform(
			Components::Position(DirectX::XMFLOAT3(
				static_cast<float>(translation[0] * metersPerUnit),
				static_cast<float>(translation[1] * metersPerUnit),
				static_cast<float>(translation[2] * metersPerUnit))),
			Components::Rotation(DirectX::XMFLOAT4(
				static_cast<float>(rotation.GetImaginary()[0]),
				static_cast<float>(rotation.GetImaginary()[1]),
				static_cast<float>(rotation.GetImaginary()[2]),
				static_cast<float>(rotation.GetReal()))),
			Components::Scale(DirectX::XMFLOAT3(
				static_cast<float>(scale[0]),
				static_cast<float>(scale[1]),
				static_cast<float>(scale[2]))));
	}

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

	static std::vector<uint32_t> SwizzleToIndices(const std::string& swizzle) {
		std::vector<uint32_t> indices;
		// skip leading dot if present
		size_t start = (!swizzle.empty() && swizzle[0] == '.') ? 1 : 0;
		indices.reserve(swizzle.size() - start);

		for (size_t i = start; i < swizzle.size(); ++i) {
			char c = static_cast<char>(std::tolower(swizzle[i]));
			switch (c) {
			case 'r': case 'x': case 'u':
				indices.push_back(0);
				break;
			case 'g': case 'y': case 'v':
				indices.push_back(1);
				break;
			case 'b': case 'z': case 'w':
				indices.push_back(2);
				break;
			case 'a': case 'q': case 't':
				indices.push_back(3);
				break;
			default:
				spdlog::warn("SwizzleToIndices: unknown component '{}', defaulting to 0", c);
				indices.push_back(0);
				break;
			}
		}
		return indices;
	}

	bool NormalTextureNeedsReconstructedZ(rhi::Format format)
	{
		switch (format) {
		case rhi::Format::BC5_UNorm:
		case rhi::Format::BC5_SNorm:
		case rhi::Format::R8G8_UNorm:
		case rhi::Format::R8G8_SNorm:
			return true;
		default:
			return false;
		}
	}

	struct ResolvedProducer {
		pxr::UsdShadeShader shader;
		pxr::TfToken        outputName;
	};

	using ResolveCacheKey = std::pair<pxr::SdfPath, pxr::TfToken>;
	struct ResolveCacheKeyHash {
		size_t operator()(ResolveCacheKey const& k) const noexcept {
			return TfHash()(k.first) ^ TfHash()(k.second);
		}
	};

	inline std::optional<ResolvedProducer>
		ResolveToShaderOutput(pxr::UsdShadeConnectableAPI c,
			pxr::TfToken outName,
			std::unordered_map<ResolveCacheKey, ResolvedProducer, ResolveCacheKeyHash>* cache = nullptr)
	{
		ResolveCacheKey key{ c.GetPrim().GetPath(), outName };
		if (cache) {
			auto it = cache->find(key);
			if (it != cache->end()) return it->second;
		}

		if (c.GetPrim().IsA<pxr::UsdShadeShader>()) {
			ResolvedProducer r{ pxr::UsdShadeShader(c.GetPrim()), outName };
			if (cache) (*cache)[key] = r;
			return r;
		}

		if (c.GetPrim().IsA<pxr::UsdShadeNodeGraph>()) {
			pxr::UsdShadeNodeGraph ng(c.GetPrim());
			pxr::UsdShadeOutput ngOut = ng.GetOutput(outName);
			if (!ngOut) return std::nullopt;

			auto sources = ngOut.GetConnectedSources();
			if (sources.empty()) return std::nullopt;

			// Only support single source for now
			const auto& s = sources[0];
			auto next = ResolveToShaderOutput(
				pxr::UsdShadeConnectableAPI(s.source.GetPrim()),
				s.sourceName,
				cache);
			if (next && cache) (*cache)[key] = *next;
			return next;
		}

		return std::nullopt;
	}

	std::string ProcessUVReader(std::optional<ResolvedProducer>& r) {
		std::string varnameStr;
		UsdShadeInput varnameInput = r->shader.GetInput(TfToken("varname"));
		auto attrs = UsdShadeUtils::GetValueProducingAttributes(varnameInput);
		if (!attrs.empty()) {
			auto& attr = attrs[0];
			bool success = attr.Get< std::string >(&varnameStr);
			if (!success) {
				TfToken t;
				if (attr.Get<TfToken>(&t)) {
					varnameStr = t.GetString();
				}
				else {
					spdlog::warn("UsdPrimvarReader_float2 varname input is not a string or token: {}", attr.GetName().GetString());
				}
			}
		}
        return varnameStr;
	}

		struct MaterialTextureBindingEntry {
			const char* inputName;
			TextureAndConstant MaterialDescription::*binding;
		};

		constexpr std::array<MaterialTextureBindingEntry, 9> kMaterialTextureBindings = {{
			{ "diffuseColor", &MaterialDescription::baseColor },
			{ "metallic", &MaterialDescription::metallic },
			{ "roughness", &MaterialDescription::roughness },
			{ "opacity", &MaterialDescription::opacity },
			{ "emissiveColor", &MaterialDescription::emissive },
			{ "normal", &MaterialDescription::normal },
			{ "displacement", &MaterialDescription::heightMap },
			{ "ambientOcclusion", &MaterialDescription::aoMap },
			{ "occlusion", &MaterialDescription::aoMap },
		}};

		struct OpenPBRTextureBindingEntry {
			const char* inputName;
			TextureAndConstant OpenPBRTextureBindings::*binding;
		};

		constexpr std::array<OpenPBRTextureBindingEntry, 6> kOpenPBRTextureBindings = {{
			{ "basecoatcolor", &OpenPBRTextureBindings::coatColor },
			{ "basecoatweight", &OpenPBRTextureBindings::coatWeight },
			{ "basecoatroughness", &OpenPBRTextureBindings::coatRoughness },
			{ "fuzzcolor", &OpenPBRTextureBindings::fuzzColor },
			{ "fuzzweight", &OpenPBRTextureBindings::fuzzWeight },
			{ "fuzzroughness", &OpenPBRTextureBindings::fuzzRoughness },
		}};

		TextureAndConstant* FindTextureBinding(MaterialDescription& result, const TfToken& name) {
			for (const auto& entry : kMaterialTextureBindings) {
				if (name == TfToken(entry.inputName)) {
					return &(result.*(entry.binding));
				}
			}

			std::string normalized;
			normalized.reserve(name.GetString().size());
			for (unsigned char ch : name.GetString()) {
				if (std::isalnum(ch)) {
					normalized.push_back(static_cast<char>(std::tolower(ch)));
				}
			}
			for (const auto& entry : kOpenPBRTextureBindings) {
				if (normalized == entry.inputName) {
					return &(result.openPBRTextures.*(entry.binding));
				}
			}

			return nullptr;
		}

		template <typename Fn>
		void ForEachMaterialTextureBinding(MaterialDescription& desc, Fn&& fn) {
			for (const auto& entry : kMaterialTextureBindings) {
				fn(desc.*(entry.binding));
			}
			for (const auto& entry : kOpenPBRTextureBindings) {
				fn(desc.openPBRTextures.*(entry.binding));
			}
		}

		template <typename Fn>
		void ForEachMaterialTextureBinding(const MaterialDescription& desc, Fn&& fn) {
			for (const auto& entry : kMaterialTextureBindings) {
				fn(desc.*(entry.binding));
			}
			for (const auto& entry : kOpenPBRTextureBindings) {
				fn(desc.openPBRTextures.*(entry.binding));
			}
		}

		std::string NormalizeObjectReyesWhitelistPath(std::string_view path)
		{
			std::string normalized;
			normalized.reserve(path.size());
			for (unsigned char ch : path) {
				char out = static_cast<char>(std::tolower(ch));
				if (out == '\\') {
					out = '/';
				}
				normalized.push_back(out);
			}
			while (!normalized.empty() && normalized.front() == '/') {
				normalized.erase(normalized.begin());
			}
			return normalized;
		}

		bool IsActiveTextureBinding(const TextureAndConstant& binding)
		{
			return binding.texture != nullptr || !binding.sourcePath.empty();
		}

		bool MaterialUsesWhitelistedTexture(
			const MaterialDescription& desc,
			const std::vector<std::string>& textureWhitelist)
		{
			if (textureWhitelist.empty()) {
				return false;
			}

			bool matched = false;
			ForEachMaterialTextureBinding(desc, [&](const TextureAndConstant& binding) {
				if (matched || binding.sourcePath.empty()) {
					return;
				}
				const std::string normalized = NormalizeObjectReyesWhitelistPath(binding.sourcePath);
				matched = std::find(textureWhitelist.begin(), textureWhitelist.end(), normalized) != textureWhitelist.end();
			});
			return matched;
		}

		std::optional<float> FindObjectReyesDisplacementScaleOverride(
			const MaterialDescription& desc,
			const std::unordered_map<std::string, float>& overrides)
		{
			if (overrides.empty()) {
				return std::nullopt;
			}

			std::optional<float> result;
			ForEachMaterialTextureBinding(desc, [&](const TextureAndConstant& binding) {
				if (result || binding.sourcePath.empty()) {
					return;
				}
				const auto it = overrides.find(NormalizeObjectReyesWhitelistPath(binding.sourcePath));
				if (it != overrides.end()) {
					result = it->second;
				}
			});
			return result;
		}

		void ApplyObjectReyesDisplacementScaleOverride(
			MaterialDescription& desc,
			const std::unordered_map<std::string, float>& overrides,
			std::string_view context)
		{
			const auto scale = FindObjectReyesDisplacementScaleOverride(desc, overrides);
			if (!scale) {
				return;
			}

			desc.heightMapScale = std::max(0.0f, *scale);
			desc.enableGeometricDisplacement = desc.heightMapScale > 0.0f;
			desc.geometricDisplacementMin = -0.5f * desc.heightMapScale;
			desc.geometricDisplacementMax = 0.5f * desc.heightMapScale;
			spdlog::info(
				"Object Reyes displacement scale override applied context='{}' material='{}' scale={}.",
				context,
				desc.name,
				desc.heightMapScale);
		}

		bool ObjectReyesAtlasBakedHeightNifListed(const InMemoryStageOptions& stageOptions)
		{
			if (stageOptions.objectReyesSurfaceSamplingMode != ObjectSurfaceSamplingMode::AtlasBakedHeight) {
				return true;
			}
			if (stageOptions.objectReyesBakedHeightMaterials.empty()) {
				return false;
			}
			return std::any_of(
				stageOptions.objectReyesBakedHeightMaterials.begin(),
				stageOptions.objectReyesBakedHeightMaterials.end(),
				[&](const ObjectReyesBakedHeightMaterialEntry& entry) {
					return entry.nifPath == stageOptions.objectReyesNifPath;
				});
		}

		bool SupportsObjectReyesGeometricDisplacementCandidate(const MaterialDescription& desc)
		{
			return desc.enableGeometricDisplacement &&
				!desc.heightMapFromBaseColorAlpha &&
				(desc.heightMap.texture != nullptr || !desc.heightMap.sourcePath.empty()) &&
				(desc.heightMap.channels.empty() || desc.heightMap.channels[0] == 0u);
		}

		bool SupportsPotentialObjectReyesHeightSidecar(const MaterialDescription& desc)
		{
			return !desc.heightMapFromBaseColorAlpha &&
				(desc.heightMap.channels.empty() || desc.heightMap.channels[0] == 0u) &&
				(!desc.baseColor.sourcePath.empty() || desc.baseColor.texture != nullptr);
		}

		void AppendTextureBindingSignature(
			std::string& signature,
			const char* slotName,
			const TextureAndConstant& binding)
		{
			signature += slotName;
			signature += '=';
			signature += NormalizeObjectReyesWhitelistPath(binding.sourcePath);
			signature += ";ch=";
			for (std::uint32_t channel : binding.channels) {
				signature += std::to_string(channel);
				signature += ',';
			}
			signature += '|';
		}

		std::string BuildMaterialTextureSignature(const MaterialDescription& desc)
		{
			std::string signature;
			signature.reserve(512);
			for (const auto& entry : kMaterialTextureBindings) {
				AppendTextureBindingSignature(signature, entry.inputName, desc.*(entry.binding));
			}
			for (const auto& entry : kOpenPBRTextureBindings) {
				AppendTextureBindingSignature(signature, entry.inputName, desc.openPBRTextures.*(entry.binding));
			}
			return signature;
		}

	std::string NormalizeUsdIdentifier(std::string value) {
		std::string normalized;
		normalized.reserve(value.size());
		for (unsigned char ch : value) {
			if (std::isalnum(ch)) {
				normalized.push_back(static_cast<char>(std::tolower(ch)));
			}
		}

		return normalized;
	}

	bool IsUsdPreviewSurfaceShaderId(const pxr::TfToken& id) {
		return NormalizeUsdIdentifier(id.GetString()) == "usdpreviewsurface";
	}

	bool IsOpenPBRShaderId(const pxr::TfToken& id) {
		return NormalizeUsdIdentifier(id.GetString()).find("openpbr") != std::string::npos;
	}

	std::optional<float> ReadFloatInputValue(const pxr::UsdShadeInput& input) {
		float floatValue = 0.0f;
		if (input.Get(&floatValue)) {
			return floatValue;
		}

		double doubleValue = 0.0;
		if (input.Get(&doubleValue)) {
			return static_cast<float>(doubleValue);
		}

		int intValue = 0;
		if (input.Get(&intValue)) {
			return static_cast<float>(intValue);
		}

		return std::nullopt;
	}

	std::optional<DirectX::XMFLOAT3> ReadFloat3InputValue(const pxr::UsdShadeInput& input) {
		pxr::GfVec3f vec3fValue;
		if (input.Get(&vec3fValue)) {
			return DirectX::XMFLOAT3(vec3fValue[0], vec3fValue[1], vec3fValue[2]);
		}

		pxr::GfVec3d vec3dValue;
		if (input.Get(&vec3dValue)) {
			return DirectX::XMFLOAT3(
				static_cast<float>(vec3dValue[0]),
				static_cast<float>(vec3dValue[1]),
				static_cast<float>(vec3dValue[2]));
		}

		return std::nullopt;
	}

	std::optional<bool> ReadBoolInputValue(const pxr::UsdShadeInput& input) {
		bool boolValue = false;
		if (input.Get(&boolValue)) {
			return boolValue;
		}

		int intValue = 0;
		if (input.Get(&intValue)) {
			return intValue != 0;
		}

		return std::nullopt;
	}

	bool IsBlack(const DirectX::XMFLOAT4& value) {
		return value.x == 0.0f && value.y == 0.0f && value.z == 0.0f;
	}

	std::string NormalizeBrniflyTexturePath(std::string value)
	{
		for (char& ch : value) {
			if (ch == '/') {
				ch = '\\';
			}
		}
		while (!value.empty() && (value.front() == '\\' || value.front() == '/')) {
			value.erase(value.begin());
		}
		std::string lower = value;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		if (!lower.empty() && lower.rfind("textures\\", 0) != 0) {
			value = "textures\\" + value;
		}
		return value;
	}

	std::string NormalizeTextureRelativePath(std::string path)
	{
		for (char& ch : path) {
			if (ch == '/') {
				ch = '\\';
			}
		}
		while (!path.empty() && (path.front() == '\\' || path.front() == '/')) {
			path.erase(path.begin());
		}
		return path;
	}

	bool HasTextureExtension(std::string_view path)
	{
		const auto dot = path.find_last_of('.');
		const auto slash = path.find_last_of("\\/");
		if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
			return false;
		}

		std::string extension(path.substr(dot));
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		return extension == ".dds" || extension == ".png" || extension == ".tga" ||
			extension == ".jpg" || extension == ".jpeg" || extension == ".bmp";
	}

	std::optional<std::string> MakeParallaxHeightSiblingPath(std::string_view diffusePath)
	{
		std::string normalized = NormalizeBrniflyTexturePath(std::string(diffusePath));
		if (!HasTextureExtension(normalized)) {
			return std::nullopt;
		}

		const auto slash = normalized.find_last_of("\\/");
		const auto dot = normalized.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
			return std::nullopt;
		}

		std::string stem = normalized.substr(0, dot);
		std::string lowerStem = stem;
		std::transform(lowerStem.begin(), lowerStem.end(), lowerStem.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		if (lowerStem.ends_with("_p")) {
			return std::nullopt;
		}

		return stem + "_p" + normalized.substr(dot);
	}

	std::optional<std::string> MakeCommunityShadersPbrDisplacementPath(std::string_view diffusePath)
	{
		std::string normalized = NormalizeBrniflyTexturePath(std::string(diffusePath));
		if (!HasTextureExtension(normalized)) {
			return std::nullopt;
		}

		std::string lower = normalized;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		if (lower.rfind("textures\\pbr\\", 0) == 0) {
			return MakeParallaxHeightSiblingPath(normalized);
		}

		const std::string texturePrefix = "textures\\";
		if (lower.rfind(texturePrefix, 0) == 0) {
			normalized.erase(0, texturePrefix.size());
			lower.erase(0, texturePrefix.size());
		}

		const auto slash = normalized.find_last_of("\\/");
		const auto dot = normalized.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
			return std::nullopt;
		}

		std::string stem = normalized.substr(0, dot);
		std::string lowerStem = lower.substr(0, dot);
		for (const std::string_view suffix : { "_d", "_diffuse", "_color" }) {
			if (lowerStem.size() > suffix.size() &&
				lowerStem.compare(lowerStem.size() - suffix.size(), suffix.size(), suffix) == 0) {
				stem.resize(stem.size() - suffix.size());
				break;
			}
		}

		return "textures\\pbr\\" + stem + "_p" + normalized.substr(dot);
	}

	std::optional<std::filesystem::path> ResolveTexturePathFromSearchRoots(const std::string& texturePath)
	{
		if (texturePath.empty()) {
			return std::nullopt;
		}

		std::error_code ec;
		const std::filesystem::path input(texturePath);
		if (std::filesystem::is_regular_file(input, ec)) {
			auto canonical = std::filesystem::weakly_canonical(input, ec);
			return ec ? input : canonical;
		}

		const std::string normalizedRelative = NormalizeTextureRelativePath(texturePath);
		std::string withoutTexturesPrefix = normalizedRelative;
		std::string lower = normalizedRelative;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		if (lower.rfind("textures\\", 0) == 0) {
			withoutTexturesPrefix = normalizedRelative.substr(std::string_view("textures\\").size());
		}

		for (const std::string& rootText : loadingCache.textureSearchRoots) {
			if (rootText.empty()) {
				continue;
			}

			const std::filesystem::path root(rootText);
			const std::array<std::filesystem::path, 4> candidates = {
				root / normalizedRelative,
				root / "textures" / withoutTexturesPrefix,
				root / "Assets" / normalizedRelative,
				root / "Assets" / "textures" / withoutTexturesPrefix
			};
			for (const auto& candidate : candidates) {
				ec.clear();
				if (std::filesystem::is_regular_file(candidate, ec)) {
					auto canonical = std::filesystem::weakly_canonical(candidate, ec);
					return ec ? candidate : canonical;
				}
			}
		}

		if (loadingCache.resolveResourcePath) {
			if (auto resolved = loadingCache.resolveResourcePath(normalizedRelative); resolved && !resolved->empty()) {
				const std::filesystem::path candidate(*resolved);
				ec.clear();
				if (std::filesystem::is_regular_file(candidate, ec)) {
					auto canonical = std::filesystem::weakly_canonical(candidate, ec);
					return ec ? candidate : canonical;
				}
			}
		}

		return std::nullopt;
	}

	void LogUnresolvedTextureOnce(const std::string& logicalPath)
	{
		static std::mutex mutex;
		static std::unordered_set<std::string> paths;
		std::string key = logicalPath;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});
		std::lock_guard lock(mutex);
		if (paths.insert(std::move(key)).second) {
			spdlog::warn("USDLoader: unable to resolve texture '{}'", logicalPath);
		}
		else {
			spdlog::debug("USDLoader: texture remains unresolved '{}'", logicalPath);
		}
	}

	void DisableModelSpaceNormalMap(MaterialDescription& result);

	void ApplyBrniflyTextureSlot(
		MaterialDescription& result,
		std::size_t slot,
		std::string path)
	{
		if (path.empty()) {
			return;
		}
		path = NormalizeBrniflyTexturePath(std::move(path));
		if (path.empty()) {
			return;
		}

		switch (slot) {
		case 0:
			result.baseColor.sourcePath = std::move(path);
			result.baseColor.channels = { 0, 1, 2, 3 };
			break;
		case 1:
			result.normal.sourcePath = std::move(path);
			if (result.brniflyModelSpaceNormals) {
				DisableModelSpaceNormalMap(result);
			} else {
				result.normal.channels = { 0, 1, 2 };
				result.negateNormals = false;
				result.invertNormalGreen = false;
			}
			break;
		case 3:
			result.emissive.sourcePath = std::move(path);
			result.emissive.channels = { 0, 1, 2 };
			if (IsBlack(result.emissiveColor)) {
				result.emissiveColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			}
			break;
		case 4:
			result.heightMap.sourcePath = std::move(path);
			result.heightMap.channels = { 0 };
			result.enableGeometricDisplacement = true;
			result.geometricDisplacementMin = std::min(result.geometricDisplacementMin, 0.0f);
			result.geometricDisplacementMax = std::max(result.geometricDisplacementMax, result.heightMapScale);
			break;
		default:
			break;
		}
	}

	void ApplyBrniflyTextureMetadata(MaterialDescription& result, const json& metadata)
	{
		const json* textures = nullptr;
		if (metadata.contains("textures") && metadata["textures"].is_array()) {
			textures = &metadata["textures"];
		} else if (metadata.is_array()) {
			textures = &metadata;
		}
		if (!textures) {
			return;
		}

		for (std::size_t slot = 0; slot < textures->size(); ++slot) {
			const auto& entry = (*textures)[slot];
			if (entry.is_string()) {
				ApplyBrniflyTextureSlot(result, slot, entry.get<std::string>());
			}
		}
	}

	std::optional<pxr::TfToken> MapOpenPBRInputToLegacyTextureSlot(const pxr::TfToken& name) {
		const std::string normalized = NormalizeUsdIdentifier(name.GetString());
		if (normalized == "basecolor") {
			return pxr::TfToken("diffuseColor");
		}
		if (normalized == "basemetalness") {
			return pxr::TfToken("metallic");
		}
		if (normalized == "specularroughness") {
			return pxr::TfToken("roughness");
		}
		if (normalized == "geometryopacity") {
			return pxr::TfToken("opacity");
		}
		if (normalized == "emissioncolor") {
			return pxr::TfToken("emissiveColor");
		}
		if (normalized == "geometrynormal" || normalized == "normal") {
			return pxr::TfToken("normal");
		}
		if (normalized == "displacement") {
			return pxr::TfToken("displacement");
		}
		if (normalized == "ambientocclusion") {
			return pxr::TfToken("ambientOcclusion");
		}
		if (normalized == "coatcolor") {
			return pxr::TfToken("basecoatcolor");
		}
		if (normalized == "coatweight") {
			return pxr::TfToken("basecoatweight");
		}
		if (normalized == "coatroughness") {
			return pxr::TfToken("basecoatroughness");
		}
		if (normalized == "fuzzcolor") {
			return pxr::TfToken("fuzzcolor");
		}
		if (normalized == "fuzzweight") {
			return pxr::TfToken("fuzzweight");
		}
		if (normalized == "fuzzroughness") {
			return pxr::TfToken("fuzzroughness");
		}

		return std::nullopt;
	}

	bool ApplyOpenPBRConstantInput(MaterialDescription& result, const pxr::UsdShadeInput& input) {
		const std::string normalized = NormalizeUsdIdentifier(input.GetBaseName().GetString());

		if (normalized == "baseweight") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.baseWeight = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "basecolor") {
			if (const auto value = ReadFloat3InputValue(input)) {
				result.openPBR.baseColor = *value;
				result.diffuseColor.x = value->x;
				result.diffuseColor.y = value->y;
				result.diffuseColor.z = value->z;
			}
			return true;
		}
		if (normalized == "basemetalness") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.baseMetalness = std::clamp(*value, 0.0f, 1.0f);
				result.metallic.factor = result.openPBR.baseMetalness;
			}
			return true;
		}
		if (normalized == "specularweight") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.specularWeight = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "specularcolor") {
			if (const auto value = ReadFloat3InputValue(input)) {
				result.openPBR.specularColor = *value;
			}
			return true;
		}
		if (normalized == "specularroughness") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.specularRoughness = std::clamp(*value, 0.0f, 1.0f);
				result.roughness.factor = result.openPBR.specularRoughness;
			}
			return true;
		}
		if (normalized == "specularior") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.specularIor = std::max(*value, 1.0f);
			}
			return true;
		}
		if (normalized == "coatweight") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.coatWeight = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "coatcolor") {
			if (const auto value = ReadFloat3InputValue(input)) {
				result.openPBR.coatColor = *value;
			}
			return true;
		}
		if (normalized == "coatroughness") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.coatRoughness = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "coatior") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.coatIor = std::max(*value, 1.0f);
			}
			return true;
		}
		if (normalized == "coatdarkening") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.coatDarkening = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "fuzzweight") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.fuzzWeight = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "fuzzcolor") {
			if (const auto value = ReadFloat3InputValue(input)) {
				result.openPBR.fuzzColor = *value;
			}
			return true;
		}
		if (normalized == "fuzzroughness") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.fuzzRoughness = std::clamp(*value, 0.0f, 1.0f);
			}
			return true;
		}
		if (normalized == "emissioncolor") {
			if (const auto value = ReadFloat3InputValue(input)) {
				result.openPBR.emissionColor = *value;
				result.emissiveColor = { value->x, value->y, value->z, 1.0f };
			}
			return true;
		}
		if (normalized == "emissionluminance") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.emissionLuminance = std::max(*value, 0.0f);
				result.emissive.factor = result.openPBR.emissionLuminance;
			}
			return true;
		}
		if (normalized == "geometryopacity") {
			if (const auto value = ReadFloatInputValue(input)) {
				result.openPBR.geometryOpacity = std::clamp(*value, 0.0f, 1.0f);
				result.opacity.factor = result.openPBR.geometryOpacity;
			}
			return true;
		}
		if (normalized == "geometrythinwalled") {
			if (const auto value = ReadBoolInputValue(input)) {
				result.openPBR.geometryThinWalled = *value;
			}
			return true;
		}

		return false;
	}

    std::vector<std::string> CollectReferencedUvSetNames(const MaterialDescription& desc) {
        std::vector<std::string> names;
        auto appendIfValid = [&](const TextureAndConstant& binding) {
            if (!binding.uvSetName.empty() &&
                std::find(names.begin(), names.end(), binding.uvSetName) == names.end()) {
                names.push_back(binding.uvSetName);
            }
        };

			ForEachMaterialTextureBinding(desc, appendIfValid);
        return names;
    }

	void MarkDisplacementEnabled(MaterialDescription& result, float displacementScale)
	{
		result.enableGeometricDisplacement = true;
		result.heightMapScale = displacementScale;
		result.geometricDisplacementMin = std::min(result.geometricDisplacementMin, 0.0f);
		result.geometricDisplacementMax = std::max(result.geometricDisplacementMax, displacementScale);
	}

	void DisableModelSpaceNormalMap(MaterialDescription& result)
	{
		result.brniflyModelSpaceNormals = true;
		result.normal.texture.reset();
		result.normal.channels.clear();
		result.negateNormals = false;
		result.invertNormalGreen = false;
	}

	bool TryGetCustomString(const UsdPrim& prim, const TfToken& key, std::string& out)
	{
		const VtValue value = prim.GetCustomDataByKey(key);
		if (!value.IsHolding<std::string>()) {
			return false;
		}
		out = value.UncheckedGet<std::string>();
		return !out.empty();
	}

	void ApplyBrniflyMaterialMetadata(MaterialDescription& result, const json& metadata)
	{
		const json* shader = nullptr;
		const json* alpha = nullptr;
		if (metadata.contains("shader") && metadata["shader"].is_object()) {
			shader = &metadata["shader"];
		} else if (metadata.contains("shaderFlags1") || metadata.contains("shaderFlags2") || metadata.contains("lightingShader")) {
			shader = &metadata;
		}
		if (metadata.contains("alpha") && metadata["alpha"].is_object()) {
			alpha = &metadata["alpha"];
		} else if (metadata.contains("flags") && metadata.contains("threshold")) {
			alpha = &metadata;
		}

		if (shader) {
			const uint32_t shaderFlags1 = shader->value("shaderFlags1", 0u);
			const uint32_t shaderFlags2 = shader->value("shaderFlags2", 0u);
			result.brniflyVertexAlpha = result.brniflyVertexAlpha || ((shaderFlags1 & (1u << 3)) != 0u);
			const bool hasParallax =
				(shaderFlags1 & (1u << 11)) != 0u ||
				(shaderFlags1 & (1u << 28)) != 0u ||
				(shaderFlags2 & (1u << 24)) != 0u;
			if (hasParallax && result.heightMap.sourcePath.empty()) {
				result.heightMapFromBaseColorAlpha = true;
				MarkDisplacementEnabled(result, result.heightMapScale);
			}
			if ((shaderFlags1 & (1u << 12)) != 0u) {
				DisableModelSpaceNormalMap(result);
			}
			result.brniflyDecal = result.brniflyDecal || ((shaderFlags1 & (1u << 26)) != 0u);
			result.brniflyDynamicDecal = result.brniflyDynamicDecal || ((shaderFlags1 & (1u << 27)) != 0u);
			if (shader->contains("shaderFlags2")) {
				result.brniflyZBufferWrite = (shaderFlags2 & 1u) != 0u;
			}
			if ((shaderFlags2 & (1u << 4)) != 0u) {
				result.forceDoubleSided = true;
			}
			if (shader->contains("lightingShader") && (*shader)["lightingShader"].is_object()) {
				const auto& lighting = (*shader)["lightingShader"];
				if (lighting.contains("alpha") && lighting["alpha"].is_number()) {
					const float alphaValue = lighting["alpha"].get<float>();
					result.opacity.factor = alphaValue;
					if (alphaValue < 1.0f) {
						result.blendState = BlendState::BLEND_STATE_BLEND;
					}
				}
			}
		}

		if (alpha) {
			const uint32_t alphaFlags = alpha->value("flags", 0u);
			const bool alphaBlend = (alphaFlags & 0x0001u) != 0u;
			const bool alphaTest = (alphaFlags & 0x0200u) != 0u;
			if (alphaTest) {
				result.blendState = BlendState::BLEND_STATE_MASK;
				result.alphaCutoff = std::clamp(alpha->value("threshold", 128u) / 255.0f, 0.0f, 1.0f);
			} else if (alphaBlend) {
				result.blendState = BlendState::BLEND_STATE_BLEND;
			}
		}
	}

	void ApplyBrniflyMaterialMetadata(MaterialDescription& result, const UsdPrim& prim)
	{
		auto applyCustomJson = [&](const TfToken& key) {
			std::string metadataJson;
			if (!TryGetCustomString(prim, key, metadataJson)) {
				return;
			}
			try {
				ApplyBrniflyMaterialMetadata(result, json::parse(metadataJson));
			}
			catch (const std::exception& ex) {
				spdlog::warn(
					"Failed to parse BRNifly material metadata '{}' on '{}': {}",
					key.GetString(),
				prim.GetPath().GetString(),
				ex.what());
			}
		};
		auto applyTextureJson = [&] {
			std::string metadataJson;
			if (!TryGetCustomString(prim, TfToken("brnifly:textures"), metadataJson)) {
				return;
			}
			try {
				ApplyBrniflyTextureMetadata(result, json::parse(metadataJson));
			}
			catch (const std::exception& ex) {
				spdlog::warn(
					"Failed to parse BRNifly texture metadata on '{}': {}",
					prim.GetPath().GetString(),
					ex.what());
			}
		};
		applyCustomJson(TfToken("brnifly:material"));
		applyTextureJson();
		applyCustomJson(TfToken("brnifly:shader"));
		applyCustomJson(TfToken("brnifly:alphaProperty"));
	}

	TextureSemantic GetTextureSemanticForUsdInput(const TfToken& name)
	{
		if (name == TfToken("diffuseColor") || name == TfToken("baseColor") || name == TfToken("coatColor") || name == TfToken("fuzzColor")) {
			return TextureSemantic::BaseColor;
		}
		if (name == TfToken("emissiveColor")) {
			return TextureSemantic::Emissive;
		}
		if (name == TfToken("normal")) {
			return TextureSemantic::Normal;
		}
		if (name == TfToken("displacement") || name == TfToken("height") || name == TfToken("heightMap")) {
			return TextureSemantic::Height;
		}
		if (name == TfToken("ambientOcclusion") || name == TfToken("occlusion")) {
			return TextureSemantic::AO;
		}
		if (name == TfToken("opacity")) {
			return TextureSemantic::Opacity;
		}
		if (name == TfToken("metallic") || name == TfToken("metalness") || name == TfToken("coatWeight") || name == TfToken("fuzzWeight")) {
			return TextureSemantic::Metallic;
		}
		if (name == TfToken("roughness") || name == TfToken("coatRoughness") || name == TfToken("fuzzRoughness")) {
			return TextureSemantic::Roughness;
		}
		return TextureSemantic::Unknown;
	}

	std::string BuildUsdTextureCacheKey(const std::string& logicalPath, TextureSemantic semantic, bool preferSRGB, NormalMapConvention normalConvention)
	{
		return logicalPath + "|semantic:" + std::to_string(static_cast<uint32_t>(semantic)) +
			(preferSRGB ? "|srgb" : "|linear") +
			"|normalconv:" + std::to_string(static_cast<uint32_t>(normalConvention));
	}

	std::string GetUsdAssetLogicalPath(const SdfAssetPath& asset)
	{
		if (!asset.GetAssetPath().empty()) {
			return asset.GetAssetPath();
		}
		return asset.GetResolvedPath();
	}

	std::shared_ptr<TextureAsset> LoadUsdTextureAsset(
		const std::string& logicalPath,
		const UsdStageRefPtr& stage,
		TextureSemantic semantic,
		bool preferSRGB,
		NormalMapConvention normalConvention)
	{
		if (logicalPath.empty()) {
			return nullptr;
		}

		const std::string cacheKey = BuildUsdTextureCacheKey(logicalPath, semantic, preferSRGB, normalConvention);
		if (auto it = loadingCache.textureCache.find(cacheKey); it != loadingCache.textureCache.end()) {
			return it->second;
		}
		if (loadingCache.unresolvedTextureCache.contains(cacheKey)) {
			return nullptr;
		}

		auto& resolver = ArGetResolver();
		auto ctx = stage ? stage->GetPathResolverContext() : ArResolverContext{};
		ArResolverContextBinder binder(ctx);

		ArResolvedPath resolved = resolver.Resolve(logicalPath);
		std::string resolvedPath = resolved.GetPathString();
		if (resolvedPath.empty()) {
			if (auto fallback = ResolveTexturePathFromSearchRoots(logicalPath)) {
				resolvedPath = fallback->string();
				resolved = ArResolvedPath(resolvedPath);
			}
		}
		if (resolvedPath.empty()) {
			loadingCache.unresolvedTextureCache.insert(cacheKey);
			LogUnresolvedTextureOnce(logicalPath);
			return nullptr;
		}

		TextureFileMeta cacheProbeMeta{};
		cacheProbeMeta.filePath = resolvedPath;
		cacheProbeMeta.preferSRGB = preferSRGB;
		cacheProbeMeta.processing = MakeMaterialTextureProcessingSettings(semantic, preferSRGB, cacheKey, false, normalConvention);

		std::shared_ptr<TextureAsset> tex;
		const std::wstring cachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(cacheProbeMeta);
		if (!cachePath.empty()) {
			TextureFileMeta deferredMeta = cacheProbeMeta;
			deferredMeta.filePath = ws2s(cachePath);
			deferredMeta.isProcessingCacheArtifact = true;
			tex = LoadTextureFromFileDeferred(cachePath, nullptr, preferSRGB, std::addressof(deferredMeta));
			if (tex) {
				tex->Meta().isProcessingCacheArtifact = true;
				spdlog::debug("USDLoader: texture processing cache hit for '{}' -> '{}'", resolvedPath, ws2s(cachePath));
			}
		}
		else if (std::error_code ec; std::filesystem::is_regular_file(resolvedPath, ec)) {
			TextureFileMeta deferredMeta = cacheProbeMeta;
			tex = LoadTextureFromFileDeferred(s2ws(resolvedPath), nullptr, preferSRGB, std::addressof(deferredMeta));
		}
		else if (std::shared_ptr<ArAsset> arAsset = resolver.OpenAsset(resolved)) {
			try {
				tex = LoadTextureFromMemory(
					static_cast<const void*>(arAsset->GetBuffer().get()),
					arAsset->GetSize(),
					nullptr,
					{},
					preferSRGB);
			}
			catch (const std::exception& ex) {
				spdlog::debug(
					"USDLoader: unable to decode texture '{}' from resolver asset memory ({}); falling back to deferred file load '{}'",
					logicalPath,
					ex.what(),
					resolvedPath);
				TextureFileMeta deferredMeta = cacheProbeMeta;
				tex = LoadTextureFromFileDeferred(s2ws(resolvedPath), nullptr, preferSRGB, std::addressof(deferredMeta));
			}
		}
		else {
			TextureFileMeta deferredMeta = cacheProbeMeta;
			tex = LoadTextureFromFileDeferred(s2ws(resolvedPath), nullptr, preferSRGB, std::addressof(deferredMeta));
		}

		if (tex) {
			tex->Meta().filePath = logicalPath;
			tex->Meta().preferSRGB = preferSRGB;
			tex->SetProcessingSettings(cacheProbeMeta.processing);
			tex->SetGenerateMipmaps(true);
			loadingCache.textureCache[cacheKey] = tex;
		}
		return tex;
	}

	void ProcessTexture(
		MaterialDescription& result,
		const UsdShadeConnectionSourceInfo& src,
		const UsdStageRefPtr& stage,
		const TfToken& name,
		const UsdShadeMaterial& material,
		bool loadMaterialTextures)
	{
		if (auto srcShader = UsdShadeShader(src.source)) {
			TfToken srcId;
			srcShader.GetIdAttr().Get(&srcId);

			if (srcId == TfToken("UsdUVTexture")) {
				// load the texture and stash it
				SdfAssetPath asset;
				srcShader.GetInput(TfToken("file")).Get(&asset);
				// Resolve asset path
				std::string logicalPath = GetUsdAssetLogicalPath(asset);

				UsdShadeInput csInput = srcShader.GetInput(TfToken("sourceColorSpace"));
				TfToken colorSpaceToken;
				std::string colorSpace = "linear";
				if (csInput && csInput.Get(&colorSpaceToken)) {
					colorSpace = colorSpaceToken.GetString();
				} // TODO: Use this to set texture color space instead of correcting in shader
				std::string csLower = colorSpace;
				std::transform(csLower.begin(), csLower.end(), csLower.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				const bool preferSRGB = (csLower == "srgb");
				const TextureSemantic semantic = GetTextureSemanticForUsdInput(name);
				const NormalMapConvention normalConvention = semantic == TextureSemantic::Normal
					? NormalMapConvention::OpenGL
					: NormalMapConvention::DirectX;
				if (loadMaterialTextures) {
					LoadUsdTextureAsset(logicalPath, stage, semantic, preferSRGB, normalConvention);
				}
			}

			// Check if this shader has an "inputs:st" input
			UsdShadeInput stInput = srcShader.GetInput(TfToken("st"));
			if (stInput) {
				if (stInput.HasConnectedSource()) {
					std::unordered_map<ResolveCacheKey, ResolvedProducer, ResolveCacheKeyHash> cache;
					auto surfSources = stInput.GetConnectedSources();
					auto resolvedSurf = ResolveToShaderOutput(
						pxr::UsdShadeConnectableAPI(surfSources[0].source.GetPrim()),
						surfSources[0].sourceName,
						&cache);
					if (resolvedSurf) {
                        if (TextureAndConstant* textureBinding = FindTextureBinding(result, name)) {
                            textureBinding->uvSetName = ProcessUVReader(resolvedSurf);
                        }
					}
					else {
						spdlog::warn("Unable to resolve 'st' input for texture shader {}", src.source.GetPrim().GetName().GetString());
					}
				}
			}
			else {
				spdlog::warn("Shader {} does not have 'st' input for UVs", src.source.GetPrim().GetName().GetString());
			}

			// now map that texture into the correct material slot:
			SdfAssetPath asset;
			srcShader.GetInput(TfToken("file")).Get(&asset);
			// Resolve asset path
			std::string logicalPath = GetUsdAssetLogicalPath(asset);
			UsdShadeInput csInput = srcShader.GetInput(TfToken("sourceColorSpace"));
			TfToken colorSpaceToken;
			std::string colorSpace = "linear";
			if (csInput && csInput.Get(&colorSpaceToken)) {
				colorSpace = colorSpaceToken.GetString();
			}
			std::transform(colorSpace.begin(), colorSpace.end(), colorSpace.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			const bool preferSRGB = (colorSpace == "srgb");
			const TextureSemantic semantic = GetTextureSemanticForUsdInput(name);
			const NormalMapConvention normalConvention = semantic == TextureSemantic::Normal
				? NormalMapConvention::OpenGL
				: NormalMapConvention::DirectX;
			const std::string cacheKey = BuildUsdTextureCacheKey(logicalPath, semantic, preferSRGB, normalConvention);
			auto texIt = loadingCache.textureCache.find(cacheKey);
			auto tex = texIt != loadingCache.textureCache.end() ? texIt->second : std::shared_ptr<TextureAsset>{};
			if (loadMaterialTextures && texIt == loadingCache.textureCache.end()) {
				return;
			}

			std::string swizzle = src.sourceName.GetString();
			TextureAndConstant* textureBinding = FindTextureBinding(result, name);
			if (textureBinding == nullptr) {
				spdlog::debug("Unknown texture input: {}", name.GetString());
				return;
			}

			textureBinding->texture = tex;
			textureBinding->sourcePath = logicalPath;
			if (tex) {
				tex->Meta().filePath = logicalPath;
				tex->Meta().preferSRGB = preferSRGB;
			}
			textureBinding->channels = SwizzleToIndices(swizzle);
			if (name == TfToken("diffuseColor") && textureBinding->channels.size() == 3) {
				textureBinding->channels.push_back(3);
			}
			if (name == TfToken("normal")) {
				if (tex && NormalTextureNeedsReconstructedZ(tex->Format())) {
					textureBinding->channels = { 0u, 1u, 4u };
				}
				if (tex) {
					result.negateNormals =
						tex->Meta().fileType == ImageFiletype::DDS ||
						(tex->Meta().isProcessingCacheArtifact && tex->Meta().processing.semantic == TextureSemantic::Normal);
				}
				result.invertNormalGreen = false;
			}
			if (name == TfToken("emissiveColor") && IsBlack(result.emissiveColor)) {
				result.emissiveColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			}
		}
	}

	void LoadSourcePathTextureBinding(
		TextureAndConstant& binding,
		const UsdStageRefPtr& stage,
		TextureSemantic semantic,
		bool preferSRGB,
		NormalMapConvention normalConvention)
	{
		if (binding.texture || binding.sourcePath.empty()) {
			return;
		}

		binding.texture = LoadUsdTextureAsset(binding.sourcePath, stage, semantic, preferSRGB, normalConvention);
	}

	void LoadSourcePathTextures(MaterialDescription& result, const UsdStageRefPtr& stage, bool loadMaterialTextures)
	{
		if (!loadMaterialTextures) {
			return;
		}

		LoadSourcePathTextureBinding(result.baseColor, stage, TextureSemantic::BaseColor, true, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.metallic, stage, TextureSemantic::Metallic, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.roughness, stage, TextureSemantic::Roughness, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.emissive, stage, TextureSemantic::Emissive, true, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.opacity, stage, TextureSemantic::Opacity, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.aoMap, stage, TextureSemantic::AO, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.heightMap, stage, TextureSemantic::Height, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.normal, stage, TextureSemantic::Normal, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.coatColor, stage, TextureSemantic::OpenPBRColor, true, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.coatWeight, stage, TextureSemantic::OpenPBRScalar, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.coatRoughness, stage, TextureSemantic::Roughness, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.fuzzColor, stage, TextureSemantic::OpenPBRColor, true, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.fuzzWeight, stage, TextureSemantic::OpenPBRScalar, false, NormalMapConvention::DirectX);
		LoadSourcePathTextureBinding(result.openPBRTextures.fuzzRoughness, stage, TextureSemantic::Roughness, false, NormalMapConvention::DirectX);

		if (result.normal.texture) {
			if (NormalTextureNeedsReconstructedZ(result.normal.texture->Format())) {
				result.normal.channels = { 0u, 1u, 4u };
			}
			result.negateNormals =
				result.normal.texture->Meta().fileType == ImageFiletype::DDS ||
				(result.normal.texture->Meta().isProcessingCacheArtifact &&
					result.normal.texture->Meta().processing.semantic == TextureSemantic::Normal);
			result.invertNormalGreen = false;
		}
	}

	bool PromoteParallaxHeightSourceFromBaseColor(MaterialDescription& result, const UsdStageRefPtr& stage, bool loadMaterialTextures)
	{
		if (result.baseColor.sourcePath.empty() ||
			(result.heightMap.texture && !result.heightMapFromBaseColorAlpha) ||
			(!result.heightMap.sourcePath.empty() && !result.heightMapFromBaseColorAlpha)) {
			return false;
		}

		std::vector<std::string> candidates;
		auto appendCandidate = [&](std::optional<std::string> candidate) {
			if (!candidate || candidate->empty()) {
				return;
			}
			const std::string normalized = NormalizeBrniflyTexturePath(*candidate);
			if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end()) {
				candidates.push_back(normalized);
			}
		};
		appendCandidate(MakeCommunityShadersPbrDisplacementPath(result.baseColor.sourcePath));
		appendCandidate(MakeParallaxHeightSiblingPath(result.baseColor.sourcePath));

		for (const std::string& candidate : candidates) {
			if (!ResolveTexturePathFromSearchRoots(candidate)) {
				continue;
			}

			result.heightMap.sourcePath = candidate;
			result.heightMap.channels = { 0 };
			result.heightMap.uvSetIndex = result.baseColor.uvSetIndex;
			result.heightMap.uvSetName = result.baseColor.uvSetName;
			result.heightMapFromBaseColorAlpha = false;
			MarkDisplacementEnabled(result, result.heightMapScale);
			if (loadMaterialTextures) {
				result.heightMap.texture = LoadUsdTextureAsset(
					candidate,
					stage,
					TextureSemantic::Height,
					false,
					NormalMapConvention::DirectX);
			}

			spdlog::info(
				"USDLoader: promoted parallax height '{}' from base color '{}'.",
				candidate,
				result.baseColor.sourcePath);
			return true;
		}

		return false;
	}

	bool PromoteParallaxHeightSourcePathFromBaseColor(MaterialDescription& result)
	{
		if (result.baseColor.sourcePath.empty() ||
			(result.heightMap.texture && !result.heightMapFromBaseColorAlpha) ||
			(!result.heightMap.sourcePath.empty() && !result.heightMapFromBaseColorAlpha)) {
			return false;
		}

		std::vector<std::string> candidates;
		auto appendCandidate = [&](std::optional<std::string> candidate) {
			if (!candidate || candidate->empty()) {
				return;
			}
			const std::string normalized = NormalizeBrniflyTexturePath(*candidate);
			if (std::find(candidates.begin(), candidates.end(), normalized) == candidates.end()) {
				candidates.push_back(normalized);
			}
		};
		appendCandidate(MakeCommunityShadersPbrDisplacementPath(result.baseColor.sourcePath));
		appendCandidate(MakeParallaxHeightSiblingPath(result.baseColor.sourcePath));

		for (const std::string& candidate : candidates) {
			if (!ResolveTexturePathFromSearchRoots(candidate)) {
				continue;
			}

			result.heightMap.sourcePath = candidate;
			result.heightMap.channels = { 0 };
			result.heightMap.uvSetIndex = result.baseColor.uvSetIndex;
			result.heightMap.uvSetName = result.baseColor.uvSetName;
			result.heightMapFromBaseColorAlpha = false;
			MarkDisplacementEnabled(result, result.heightMapScale);
			return true;
		}

		return false;
	}

	void ProcessDisplacementTerminal(
		MaterialDescription& result,
		const pxr::UsdShadeMaterial& material,
		const UsdStageRefPtr& stage,
		std::unordered_map<ResolveCacheKey, ResolvedProducer, ResolveCacheKeyHash>& cache,
		bool loadMaterialTextures)
	{
		pxr::UsdShadeOutput displacementOut = material.GetDisplacementOutput(pxr::UsdShadeTokens->universalRenderContext);
		if (!displacementOut) {
			return;
		}

		auto displacementSources = displacementOut.GetConnectedSources();
		if (displacementSources.empty()) {
			return;
		}

		for (auto const& src : displacementSources) {
			auto resolved = ResolveToShaderOutput(
				pxr::UsdShadeConnectableAPI(src.source.GetPrim()),
				src.sourceName,
				&cache);

			if (!resolved) {
				continue;
			}

			pxr::TfToken prodId;
			resolved->shader.GetIdAttr().Get(&prodId);

			if (prodId == pxr::TfToken("UsdUVTexture")) {
				MarkDisplacementEnabled(result, result.heightMapScale);
				ProcessTexture(result, src, stage, TfToken("displacement"), material, loadMaterialTextures);
				continue;
			}

			for (auto const& input : resolved->shader.GetInputs()) {
				const pxr::TfToken inputName = input.GetBaseName();

				if ((inputName == pxr::TfToken("scale") || inputName == pxr::TfToken("displacement")) &&
					input.GetConnectedSources().empty()) {
					float scale = result.heightMapScale;
					if (input.Get(&scale)) {
						MarkDisplacementEnabled(result, scale);
					}
					continue;
				}

				if (inputName != pxr::TfToken("displacement") && inputName != pxr::TfToken("in") && inputName != pxr::TfToken("texture")) {
					continue;
				}

				for (auto const& inputSource : input.GetConnectedSources()) {
					auto resolvedInput = ResolveToShaderOutput(
						pxr::UsdShadeConnectableAPI(inputSource.source.GetPrim()),
						inputSource.sourceName,
						&cache);

					if (!resolvedInput) {
						continue;
					}

					pxr::TfToken inputProdId;
					resolvedInput->shader.GetIdAttr().Get(&inputProdId);
					if (inputProdId == pxr::TfToken("UsdUVTexture")) {
						MarkDisplacementEnabled(result, result.heightMapScale);
						ProcessTexture(result, inputSource, stage, pxr::TfToken("displacement"), material, loadMaterialTextures);
					}
				}
			}
		}
	}

	MaterialDescription ParseMaterialGraph(
		const pxr::UsdShadeMaterial& material,
		const std::string& directory,
		const UsdStageRefPtr& stage,
		bool isUSDZ,
		bool loadMaterialTextures)
	{
		MaterialDescription result;
		
		// Get terminal output
		pxr::UsdShadeOutput surfOut =
			material.GetSurfaceOutput(pxr::UsdShadeTokens->universalRenderContext);
		if (!surfOut) return result;

		// Find the bound surface shader
		auto surfSources = surfOut.GetConnectedSources();
		if (surfSources.empty()) return result;

		// Resolve the surface producer to a shader so we can enumerate its inputs
		std::unordered_map<ResolveCacheKey, ResolvedProducer, ResolveCacheKeyHash> cache;
		auto resolvedSurf = ResolveToShaderOutput(
			pxr::UsdShadeConnectableAPI(surfSources[0].source.GetPrim()),
			surfSources[0].sourceName,
			&cache);

		if (!resolvedSurf) return result;

		pxr::UsdShadeShader surfaceShader = resolvedSurf->shader;

		// Check supported material terminal type, then parse inputs
		pxr::TfToken id;
		if (!surfaceShader.GetIdAttr().Get(&id))
			return result;

		result.name = material.GetPrim().GetName().GetString();
		result.invertNormalGreen = false;
		result.negateNormals = false;
        result.alphaCutoff = 0.0f;

		const bool isPreviewSurface = IsUsdPreviewSurfaceShaderId(id);
		const bool isOpenPBRSurface = IsOpenPBRShaderId(id);
		if (!isPreviewSurface && !isOpenPBRSurface) {
			spdlog::warn("Unsupported surface shader '{}' in material {}", id.GetString(), material.GetPrim().GetPath().GetString());
			return result;
		}

		if (isOpenPBRSurface) {
			result.materialModel = MaterialModel::OpenPBR;
			result.emissiveColor = { 1.0f, 1.0f, 1.0f, 1.0f };
			result.emissive.factor = 0.0f;
		}

		for (auto const& input : surfaceShader.GetInputs()) {
			const auto name = input.GetBaseName();

			// Read constants if unconnected
			if (input.GetConnectedSources().empty()) {
				if (isOpenPBRSurface && ApplyOpenPBRConstantInput(result, input)) {
					continue;
				}

				TfToken texName = input.GetBaseName();
				if (texName == TfToken("diffuseColor") && input.GetConnectedSources().empty()) {
					GfVec3f c; input.Get(&c);
					result.diffuseColor = { c[0],c[1],c[2],1.0f };
				}
				else if (texName == TfToken("metallic") && input.GetConnectedSources().empty()) {
					float v; input.Get(&v);
					result.metallic.factor = v;
				}
				else if (texName == TfToken("roughness") && input.GetConnectedSources().empty()) {
					float v; input.Get(&v);
					result.roughness.factor = v;
				}
				else if (texName == TfToken("opacity") && input.GetConnectedSources().empty()) {
					float v; input.Get(&v);
					result.opacity.factor = v;
				}
				else if (texName == TfToken("emissiveColor") && input.GetConnectedSources().empty()) {
					GfVec3f c; input.Get(&c);
					result.emissiveColor = { c[0],c[1],c[2],1.0f };
				}
				else if (texName == TfToken("opacityThreshold") && input.GetConnectedSources().empty()) {
					float v; input.Get(&v);
					result.alphaCutoff = v;
				}
				else if (texName == TfToken("displacement") && input.GetConnectedSources().empty()) {
					float v; input.Get(&v);
					if (v != 0.0f) {
						MarkDisplacementEnabled(result, v);
					}
				}
				else {
					spdlog::debug(
						"Unknown input '{}' with no connections in {}",
						name.GetString(),
						isOpenPBRSurface ? "OpenPBR surface" : "UsdPreviewSurface");
				}
				continue;
			}

			// For each connection, normalize to the real producer shader
			for (auto const& src : input.GetConnectedSources()) {

				auto r = ResolveToShaderOutput(
					pxr::UsdShadeConnectableAPI(src.source.GetPrim()),
					src.sourceName,
					&cache);

				if (!r) continue;

				pxr::TfToken prodId;
				r->shader.GetIdAttr().Get(&prodId);

				const std::optional<pxr::TfToken> legacyTextureName =
					isOpenPBRSurface ? MapOpenPBRInputToLegacyTextureSlot(name) : std::optional<pxr::TfToken>(name);

				if (prodId == pxr::TfToken("UsdUVTexture")) {
					if (!legacyTextureName.has_value()) {
						spdlog::warn("Unsupported OpenPBR texture input '{}' in material {}", name.GetString(), material.GetPrim().GetPath().GetString());
						continue;
					}
					if (*legacyTextureName == TfToken("displacement")) {
						MarkDisplacementEnabled(result, result.heightMapScale);
					}
					ProcessTexture(result, src, stage, *legacyTextureName, material, loadMaterialTextures);
				}
				else if (prodId == pxr::TfToken("UsdPrimvarReader_float2")) {
					if (legacyTextureName.has_value()) {
						if (TextureAndConstant* textureBinding = FindTextureBinding(result, *legacyTextureName)) {
							textureBinding->uvSetName = ProcessUVReader(r);
						}
					}
					else if (isOpenPBRSurface) {
						spdlog::warn("Unsupported OpenPBR primvar input '{}' in material {}", name.GetString(), material.GetPrim().GetPath().GetString());
					}
				}
				else {
					spdlog::warn("Unsupported shader producer: {} in material {}", prodId.GetString(), material.GetPrim().GetPath().GetString());
				}
			}
		}

        ProcessDisplacementTerminal(result, material, stage, cache, loadMaterialTextures);

		//Post-process to assign 1.0 to undefined factors with a valid texture
		ForEachMaterialTextureBinding(result, [](TextureAndConstant& binding) {
			if (binding.texture && !binding.factor.HasValue()) {
				binding.factor = 1.0f; // Unlike glTF, USD does not require a factor to be set if a texture is present
			}
		});

		ApplyBrniflyMaterialMetadata(result, material.GetPrim());
		PromoteParallaxHeightSourceFromBaseColor(result, stage, loadMaterialTextures);
		LoadSourcePathTextures(result, stage, loadMaterialTextures);

		ForEachMaterialTextureBinding(result, [](TextureAndConstant& binding) {
			if (binding.texture && !binding.factor.HasValue()) {
				binding.factor = 1.0f;
			}
		});

        spdlog::debug(
            "USD material '{}' displacement: enabled={}, hasHeightMap={}, scale={}, range=[{}, {}]",
            result.name,
            result.enableGeometricDisplacement,
            result.heightMap.texture != nullptr,
            result.heightMapScale,
            result.geometricDisplacementMin,
            result.geometricDisplacementMax);

		return result;
	}

	void ProcessMaterial(
		const pxr::UsdShadeMaterial& material,
		const pxr::UsdStageRefPtr& stage,
		const InMemoryStageOptions& stageOptions,
		bool isUSDZ,
		const std::string& directory,
		bool loadMaterialTextures)
	{
		ZoneScopedN("USDLoader::ProcessMaterial");
		if (!material) {
			return;
		}

		const auto materialPath = material.GetPrim().GetPath().GetString();
		ZoneText(materialPath.data(), materialPath.size());
		if (loadingCache.materialTemplateCache.contains(material.GetPrim().GetPath().GetString())) {
			spdlog::debug("Material {} already processed, skipping.", material.GetPrim().GetPath().GetString());
			return; // Already processed
		}

		spdlog::debug("Processing material: {}", material.GetPrim().GetPath().GetString());

		MaterialDescription materialDesc;
		{
			ZoneScopedN("USDLoader::ProcessMaterial::ParseMaterialGraph");
			materialDesc = ParseMaterialGraph(material, directory, stage, isUSDZ, loadMaterialTextures);
		}
		ApplyObjectReyesDisplacementScaleOverride(
			materialDesc,
			stageOptions.objectReyesDisplacementScaleOverrides,
			materialPath);
        MaterialTemplateRecord record;
        record.desc = std::move(materialDesc);
		{
			ZoneScopedN("USDLoader::ProcessMaterial::CollectReferencedUvSetNames");
			record.referencedUvSetNames = CollectReferencedUvSetNames(record.desc);
		}
		loadingCache.materialTemplateCache[material.GetPrim().GetPath().GetString()] = std::move(record);
	}

    uint32_t ResolveUvSetIndexForBinding(const TextureAndConstant& binding, const std::vector<MeshUvSetData>& uvSets, const std::string& materialPath, const char* slotName) {
        if (binding.uvSetName.empty()) {
            return binding.uvSetIndex;
        }

        for (uint32_t uvSetIndex = 0; uvSetIndex < uvSets.size(); ++uvSetIndex) {
            if (uvSets[uvSetIndex].name == binding.uvSetName) {
                return uvSetIndex;
            }
        }

        spdlog::error("USD material '{}' references missing UV set '{}' for slot '{}'. Falling back to UV set 0.", materialPath, binding.uvSetName, slotName);
        return 0;
    }

	std::vector<MeshUvSetData> BuildMaterialUvSetDescriptors(const UsdShadeMaterial& material)
	{
		std::vector<MeshUvSetData> uvSets;
		auto appendUnique = [&](const std::string& uvSetName) {
			if (uvSetName.empty()) {
				return;
			}
			const auto existing = std::find_if(
				uvSets.begin(),
				uvSets.end(),
				[&](const MeshUvSetData& uvSet) { return uvSet.name == uvSetName; });
			if (existing == uvSets.end()) {
				uvSets.push_back(MeshUvSetData{ .name = uvSetName });
			}
		};

		if (!material) {
			appendUnique("st");
			return uvSets;
		}

		const auto templateIt = loadingCache.materialTemplateCache.find(material.GetPrim().GetPath().GetString());
		if (templateIt == loadingCache.materialTemplateCache.end()) {
			appendUnique("st");
			return uvSets;
		}

		for (const std::string& uvSetName : templateIt->second.referencedUvSetNames) {
			appendUnique(uvSetName);
		}
		appendUnique("st");
		return uvSets;
	}

    std::string BuildResolvedMaterialCacheKey(const std::string& materialPath, const MaterialDescription& resolvedDesc) {
        return materialPath + "|" +
            std::to_string(resolvedDesc.baseColor.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.normal.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.metallic.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.roughness.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.emissive.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.aoMap.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.heightMap.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.opacity.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.coatColor.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.coatWeight.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.coatRoughness.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.fuzzColor.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.fuzzWeight.uvSetIndex) + "|" +
			std::to_string(resolvedDesc.openPBRTextures.fuzzRoughness.uvSetIndex) + "|" +
            std::to_string(resolvedDesc.forceDoubleSided ? 1 : 0) + "|" +
			std::to_string(static_cast<int>(resolvedDesc.blendState)) + "|" +
			std::to_string(resolvedDesc.alphaCutoff) + "|" +
			std::to_string(resolvedDesc.opacity.factor.Get()) + "|" +
			std::to_string(resolvedDesc.brniflyVertexAlpha ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.brniflyZBufferWrite ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.brniflyDecal ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.brniflyDynamicDecal ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.brniflyModelSpaceNormals ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.geometricDisplacementOptIn ? 1 : 0) + "|" +
			std::to_string(static_cast<std::uint32_t>(resolvedDesc.objectSurfaceSamplingMode)) + "|" +
			std::to_string(resolvedDesc.objectSurfaceUseTriplanarProjection ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.objectSurfaceUseTripleTapStochastic ? 1 : 0) + "|" +
			std::to_string(resolvedDesc.objectSurfaceTexelDensity) + "|" +
			resolvedDesc.staticTextureOverrideSourceName;
    }

    std::shared_ptr<Material> ResolveDefaultUsdMaterial(bool forceDoubleSided) {
        MaterialDescription desc = {};
        desc.name = forceDoubleSided ? "UsdDefaultPreviewMaterial" : "UsdDefaultMaterial";
        desc.forceDoubleSided = forceDoubleSided;
        const std::string cacheKey = BuildResolvedMaterialCacheKey(desc.name, desc);
        auto resolvedIt = loadingCache.resolvedMaterialCache.find(cacheKey);
        if (resolvedIt != loadingCache.resolvedMaterialCache.end()) {
            return resolvedIt->second;
        }

        auto runtimeMaterial = Material::CreateShared(desc);
        loadingCache.resolvedMaterialCache[cacheKey] = runtimeMaterial;
        return runtimeMaterial;
    }

	bool IsUnsupportedBrNiflySkinnedMesh(const UsdGeomMesh& mesh)
	{
		(void)mesh;
		return false;
	}

	bool IsBrNiflyCollisionMesh(const UsdGeomMesh& mesh)
	{
		return mesh && !mesh.GetPrim().GetCustomDataByKey(TfToken("brnifly:collision")).IsEmpty();
	}

	bool BrNiflyLODShapeImportEnabled()
	{
		char* value = nullptr;
		size_t valueSize = 0;
		if (_dupenv_s(&value, &valueSize, "BASICRENDERER_IMPORT_BRNIFLY_LOD_SHAPES") == 0 && value) {
			const bool enabled =
				_stricmp(value, "1") == 0 ||
				_stricmp(value, "true") == 0 ||
				_stricmp(value, "yes") == 0 ||
				_stricmp(value, "on") == 0;
			std::free(value);
			return enabled;
		}
		return false;
	}

	std::string GetPrimCustomString(const UsdPrim& prim, const TfToken& key)
	{
		const VtValue value = prim.GetCustomDataByKey(key);
		if (value.IsEmpty() || !value.IsHolding<std::string>()) {
			return {};
		}
		return value.UncheckedGet<std::string>();
	}

	std::optional<int> GetPrimCustomInt(const UsdPrim& prim, const TfToken& key)
	{
		const VtValue value = prim.GetCustomDataByKey(key);
		if (value.IsEmpty()) {
			return std::nullopt;
		}
		if (value.IsHolding<int>()) {
			return value.UncheckedGet<int>();
		}
		if (value.IsHolding<unsigned int>()) {
			return static_cast<int>(value.UncheckedGet<unsigned int>());
		}
		return std::nullopt;
	}

	bool IsBrNiflyObjectRootPrim(const UsdPrim& prim)
	{
		if (!prim) {
			return false;
		}
		const auto blockId = GetPrimCustomInt(prim, TfToken("brnifly:blockId"));
		if (!blockId || *blockId != 0) {
			return false;
		}
		if (GetPrimCustomString(prim, TfToken("brnifly:blockName")).empty()) {
			return false;
		}

		const auto parent = prim.GetParent();
		return parent && parent.GetName() == TfToken("BRNifly");
	}

	UsdPrim GetImmediateChildOnPath(const UsdPrim& ancestor, const UsdPrim& descendant)
	{
		if (!ancestor || !descendant) {
			return {};
		}
		const SdfPath ancestorPath = ancestor.GetPath();
		SdfPath childPath = descendant.GetPath();
		if (childPath == ancestorPath || !childPath.HasPrefix(ancestorPath)) {
			return {};
		}
		while (!childPath.IsEmpty() && childPath.GetParentPath() != ancestorPath) {
			childPath = childPath.GetParentPath();
		}
		return childPath.IsEmpty() ? UsdPrim() : ancestor.GetStage()->GetPrimAtPath(childPath);
	}

	bool IsBelowInactiveBrNiflyLOD0Branch(const UsdPrim& prim)
	{
		if (!prim) {
			return false;
		}
		UsdStageWeakPtr stage = prim.GetStage();
		if (!stage) {
			return false;
		}

		for (SdfPath ancestorPath = prim.GetPath().GetParentPath();
			!ancestorPath.IsEmpty() && ancestorPath != SdfPath::AbsoluteRootPath();
			ancestorPath = ancestorPath.GetParentPath()) {
			const UsdPrim ancestor = stage->GetPrimAtPath(ancestorPath);
			const std::optional<int> lod0ChildBlockId = GetPrimCustomInt(ancestor, TfToken("brnifly:lod0ChildBlockId"));
			if (!lod0ChildBlockId) {
				continue;
			}

			const UsdPrim branchRoot = GetImmediateChildOnPath(ancestor, prim);
			const std::optional<int> branchBlockId = GetPrimCustomInt(branchRoot, TfToken("brnifly:blockId"));
			if (branchBlockId && *branchBlockId != *lod0ChildBlockId) {
				return true;
			}
		}
		return false;
	}

	bool IsBrNiflyLODMeshName(std::string name)
	{
		std::ranges::transform(name, name.begin(), [](unsigned char ch) {
			return static_cast<char>(std::tolower(ch));
		});

		return name.starts_with("lod_") ||
			name.starts_with("billboard_") ||
			name.ends_with("_lod") ||
			name.ends_with("_lod_0") ||
			name.ends_with("_lod_1") ||
			name.ends_with("_lod_2") ||
			name.ends_with("_lod_3") ||
			name.contains("_lod_");
	}

	bool IsBrNiflyLODRenderMesh(const UsdGeomMesh& mesh)
	{
		if (!mesh || BrNiflyLODShapeImportEnabled()) {
			return false;
		}

		const auto& prim = mesh.GetPrim();
		if (IsBelowInactiveBrNiflyLOD0Branch(prim)) {
			return true;
		}
		// if (IsBrNiflyLODMeshName(prim.GetName().GetString())) {
		// 	return true;
		// }

		const std::string blockName = GetPrimCustomString(prim, TfToken("brnifly:blockName"));
		if (blockName == "BSLODTriShape") {
			return true;
		}

		const std::string shaderText = GetPrimCustomString(prim, TfToken("brnifly:shader"));
		if (shaderText.empty()) {
			return false;
		}

		try {
			const auto shader = json::parse(shaderText);
			if (shader.value("blockName", std::string{}) == "DistantLODShaderProperty") {
				return true;
			}
			if (shader.value("shaderTypeName", std::string{}) == "LODOBJECTS" ||
				shader.value("shaderTypeName", std::string{}) == "LODOBJECTSHD") {
				return true;
			}
		}
		catch (const std::exception&) {
		}

		return false;
	}

	std::vector<std::string> ParseJsonStringArray(std::string_view text)
	{
		std::vector<std::string> values;
		bool inString = false;
		bool escape = false;
		std::string current;
		for (const char ch : text) {
			if (!inString) {
				if (ch == '"') {
					inString = true;
					current.clear();
				}
				continue;
			}

			if (escape) {
				switch (ch) {
				case '"':
				case '\\':
				case '/':
					current.push_back(ch);
					break;
				case 'n':
					current.push_back('\n');
					break;
				case 'r':
					current.push_back('\r');
					break;
				case 't':
					current.push_back('\t');
					break;
				default:
					current.push_back(ch);
					break;
				}
				escape = false;
				continue;
			}

			if (ch == '\\') {
				escape = true;
				continue;
			}
			if (ch == '"') {
				values.push_back(current);
				inString = false;
				continue;
			}
			current.push_back(ch);
		}
		return values;
	}

	std::vector<std::string> GetBrNiflyJointNames(const UsdPrim& prim)
	{
		if (!prim) {
			return {};
		}
		const VtValue value = prim.GetCustomDataByKey(TfToken("brnifly:jointNames"));
		if (!value.IsHolding<std::string>()) {
			return {};
		}
		return ParseJsonStringArray(value.UncheckedGet<std::string>());
	}

	std::vector<float> ParseJsonFloatArray(std::string_view text)
	{
		std::vector<float> values;
		const char* begin = text.data();
		const char* const end = begin + text.size();
		while (begin < end) {
			char* parsedEnd = nullptr;
			const float value = std::strtof(begin, &parsedEnd);
			if (parsedEnd != begin) {
				values.push_back(value);
				begin = parsedEnd;
				continue;
			}
			++begin;
		}
		return values;
	}

	std::vector<std::uint32_t> ParseJsonUIntArray(std::string_view text)
	{
		std::vector<std::uint32_t> values;
		const char* begin = text.data();
		const char* const end = begin + text.size();
		while (begin < end) {
			char* parsedEnd = nullptr;
			const long value = std::strtol(begin, &parsedEnd, 10);
			if (parsedEnd != begin) {
				if (value >= 0) {
					values.push_back(static_cast<std::uint32_t>(value));
				}
				begin = parsedEnd;
				continue;
			}
			++begin;
		}
		return values;
	}

	std::vector<std::uint32_t> GetBrNiflyJointSourceIndices(const UsdPrim& prim)
	{
		if (!prim) {
			return {};
		}
		const VtValue value = prim.GetCustomDataByKey(TfToken("brnifly:jointSourceIndices"));
		if (!value.IsHolding<std::string>()) {
			return {};
		}
		return ParseJsonUIntArray(value.UncheckedGet<std::string>());
	}

	std::vector<XMMATRIX> GetBrNiflySkinToBoneTransforms(const UsdPrim& prim)
	{
		if (!prim) {
			return {};
		}
		const VtValue value = prim.GetCustomDataByKey(TfToken("brnifly:skinToBoneTransforms"));
		if (!value.IsHolding<std::string>()) {
			return {};
		}

		constexpr size_t kTransformFloatCount = 13;
		const auto floats = ParseJsonFloatArray(value.UncheckedGet<std::string>());
		if (floats.empty() || (floats.size() % kTransformFloatCount) != 0u) {
			return {};
		}

		std::vector<XMMATRIX> matrices;
		matrices.reserve(floats.size() / kTransformFloatCount);
		for (size_t offset = 0; offset + kTransformFloatCount <= floats.size(); offset += kTransformFloatCount) {
			const float* x = floats.data() + offset;
			const float scale = x[12];
			// Nifly exports MatTransform as translation, row-major rotation, scale.
			// Match the row-vector NiTransform snapshot layout used by the live Skyrim bridge.
			matrices.push_back(XMMATRIX(
				x[3] * scale, x[6] * scale, x[9] * scale, 0.0f,
				x[4] * scale, x[7] * scale, x[10] * scale, 0.0f,
				x[5] * scale, x[8] * scale, x[11] * scale, 0.0f,
				x[0], x[1], x[2], 1.0f));
		}
		return matrices;
	}

    std::shared_ptr<Material> ResolveMaterialForMesh(
		const UsdShadeMaterial& material,
		const std::vector<MeshUvSetData>& uvSets,
		bool forceDoubleSided = false,
		const UsdPrim& meshPrim = UsdPrim(),
		const MeshPreprocessResult* preprocessResult = nullptr,
		std::string_view staticTextureOverrideSourceName = {}) {
        if (!material) {
            return ResolveDefaultUsdMaterial(forceDoubleSided);
        }

        const std::string materialPath = material.GetPrim().GetPath().GetString();
        auto templateIt = loadingCache.materialTemplateCache.find(materialPath);
        if (templateIt == loadingCache.materialTemplateCache.end()) {
            return ResolveDefaultUsdMaterial(forceDoubleSided);
        }

        MaterialDescription resolvedDesc = templateIt->second.desc;
        resolvedDesc.baseColor.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.baseColor, uvSets, materialPath, "baseColor");
        resolvedDesc.normal.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.normal, uvSets, materialPath, "normal");
        resolvedDesc.metallic.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.metallic, uvSets, materialPath, "metallic");
        resolvedDesc.roughness.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.roughness, uvSets, materialPath, "roughness");
        resolvedDesc.emissive.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.emissive, uvSets, materialPath, "emissive");
        resolvedDesc.aoMap.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.aoMap, uvSets, materialPath, "ambientOcclusion");
        resolvedDesc.heightMap.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.heightMap, uvSets, materialPath, "heightMap");
        resolvedDesc.opacity.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.opacity, uvSets, materialPath, "opacity");
		resolvedDesc.openPBRTextures.coatColor.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.coatColor, uvSets, materialPath, "coatColor");
		resolvedDesc.openPBRTextures.coatWeight.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.coatWeight, uvSets, materialPath, "coatWeight");
		resolvedDesc.openPBRTextures.coatRoughness.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.coatRoughness, uvSets, materialPath, "coatRoughness");
		resolvedDesc.openPBRTextures.fuzzColor.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.fuzzColor, uvSets, materialPath, "fuzzColor");
		resolvedDesc.openPBRTextures.fuzzWeight.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.fuzzWeight, uvSets, materialPath, "fuzzWeight");
		resolvedDesc.openPBRTextures.fuzzRoughness.uvSetIndex = ResolveUvSetIndexForBinding(resolvedDesc.openPBRTextures.fuzzRoughness, uvSets, materialPath, "fuzzRoughness");
        if (meshPrim) {
            ApplyBrniflyMaterialMetadata(resolvedDesc, meshPrim);
        }
		if (!staticTextureOverrideSourceName.empty()) {
			resolvedDesc.staticTextureOverrideSourceName = std::string(staticTextureOverrideSourceName);
		}
		else if (meshPrim) {
			resolvedDesc.staticTextureOverrideSourceName = meshPrim.GetName().GetString();
		}
		if (preprocessResult && preprocessResult->geometricDisplacementOptIn) {
			resolvedDesc.geometricDisplacementOptIn = true;
		}
		if (preprocessResult && preprocessResult->objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::None) {
			resolvedDesc.objectSurfaceSamplingMode = preprocessResult->objectSurfaceSamplingMode;
		}
		if (preprocessResult) {
			resolvedDesc.objectSurfaceUseTriplanarProjection = preprocessResult->objectSurfaceUseTriplanarProjection;
			resolvedDesc.objectSurfaceUseTripleTapStochastic = preprocessResult->objectSurfaceUseTripleTapStochastic;
		}
		if (preprocessResult && preprocessResult->objectAtlasBakedHeight) {
			resolvedDesc.heightMap.texture.reset();
			resolvedDesc.heightMap.sourcePath.clear();
			resolvedDesc.heightMap.channels = { 0u };
			resolvedDesc.heightMapFromBaseColorAlpha = false;
			resolvedDesc.geometricDisplacementOptIn = true;
			resolvedDesc.enableGeometricDisplacement = true;
			resolvedDesc.heightMap.uvSetIndex = preprocessResult->objectAtlasHeightUvSetIndex;
			resolvedDesc.heightMap.uvSetName = "__object_reyes_atlas_height";
			resolvedDesc.heightMapScale = 1.0f;
			resolvedDesc.geometricDisplacementMin = preprocessResult->objectAtlasDisplacementMin;
			resolvedDesc.geometricDisplacementMax = preprocessResult->objectAtlasDisplacementMax;
		}
		if (preprocessResult) {
			resolvedDesc.objectSurfaceTexelDensity = preprocessResult->objectSurfaceTexelDensity;
		}
        resolvedDesc.forceDoubleSided = resolvedDesc.forceDoubleSided || forceDoubleSided;

        const std::string cacheKey = BuildResolvedMaterialCacheKey(materialPath, resolvedDesc);
        auto resolvedIt = loadingCache.resolvedMaterialCache.find(cacheKey);
        if (resolvedIt != loadingCache.resolvedMaterialCache.end()) {
            return resolvedIt->second;
        }

        auto runtimeMaterial = Material::CreateShared(resolvedDesc);
		if (resolvedDesc.objectSurfaceSamplingMode == ObjectSurfaceSamplingMode::AtlasBakedHeight) {
			const auto materialData = runtimeMaterial->GetData();
			spdlog::info(
				"Object Reyes atlas material created name='{}' height='{}' flags=0x{:x} compileFlags=0x{:x} rasterFlags=0x{:x} geomEnabled={} heightUv={} heightSourcePathOnly={}.",
				resolvedDesc.name,
				resolvedDesc.heightMap.sourcePath,
				materialData.materialFlags,
				static_cast<std::uint64_t>(runtimeMaterial->Technique().compileFlags),
				static_cast<std::uint32_t>(runtimeMaterial->Technique().rasterFlags),
				materialData.geometricDisplacementEnabled,
				materialData.heightUvSetIndex,
				resolvedDesc.heightMap.texture ? 0 : 1);
		}
        loadingCache.resolvedMaterialCache[cacheKey] = runtimeMaterial;
        return runtimeMaterial;
    }

	std::shared_ptr<const Mesh::ObjectReyesAtlasBakeData> BuildObjectReyesAtlasBakeDataForMesh(
		const MeshPreprocessResult& result)
	{
		if (!result.objectAtlasBakedHeight) {
			return nullptr;
		}
		if (result.objectAtlasSharedBakeData) {
			return result.objectAtlasSharedBakeData;
		}
		const std::vector<std::byte>& vertices = result.ingest.GetVertices();
		const std::vector<std::uint32_t>& indices = result.ingest.GetIndices();
		const std::vector<MeshUvSetData>& uvSets = result.ingest.GetUvSets();
		const std::uint32_t vertexSize = result.ingest.GetVertexSize();
		const std::uint32_t vertexFlags = result.ingest.GetFlags();
		if (vertexSize == 0u ||
			result.objectAtlasHeightUvSetIndex >= uvSets.size() ||
			uvSets[result.objectAtlasHeightUvSetIndex].values.empty()) {
			return nullptr;
		}
		const std::size_t vertexCount = vertices.size() / static_cast<std::size_t>(vertexSize);
		if (vertexCount == 0u ||
			uvSets[result.objectAtlasHeightUvSetIndex].values.size() != vertexCount) {
			return nullptr;
		}

		auto data = std::make_shared<Mesh::ObjectReyesAtlasBakeData>();
		data->atlasWidth = result.objectAtlasWidth;
		data->atlasHeight = result.objectAtlasHeight;
		data->atlasUvSetIndex = result.objectAtlasHeightUvSetIndex;
		data->texelsPerUnit = result.objectAtlasTexelsPerUnit;
		data->blendWidthObjectUnits = result.objectAtlasBlendWidthObjectUnits;
		data->indices = indices;
		data->triangleMaterialIndices = result.objectAtlasTriangleMaterialIndices;
		data->sourceMaterialNames = result.objectAtlasSourceMaterialNames;
		data->sourceMaterials = result.objectAtlasSourceMaterials;
		data->positions.resize(vertexCount);
		data->normals.resize(vertexCount, DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f });
		data->atlasUvs = uvSets[result.objectAtlasHeightUvSetIndex].values;
		data->uvSets.clear();
		data->uvSets.reserve(uvSets.size());
		for (const MeshUvSetData& uvSet : uvSets) {
			data->uvSets.push_back(uvSet.values);
		}
		const bool hasNormals =
			(vertexFlags & VertexFlags::VERTEX_NORMALS) != 0u &&
			vertexSize >= MeshVertexLayout::NormalOffset + sizeof(DirectX::XMFLOAT3);
		for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
			const std::byte* vertex = vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize);
			std::memcpy(std::addressof(data->positions[vertexIndex]), vertex + MeshVertexLayout::PositionOffset, sizeof(DirectX::XMFLOAT3));
			if (hasNormals) {
				std::memcpy(std::addressof(data->normals[vertexIndex]), vertex + MeshVertexLayout::NormalOffset, sizeof(DirectX::XMFLOAT3));
			}
		}
		return data;
	}

	USDGeometryExtractor::ExtractOptions BuildGeometryExtractOptions(
		const UsdGeomMesh& mesh,
		const UsdShadeMaterial& material,
		const InMemoryStageOptions& stageOptions)
	{
		MaterialDescription desc{};
		if (material) {
			const std::string materialPath = material.GetPrim().GetPath().GetString();
			const auto templateIt = loadingCache.materialTemplateCache.find(materialPath);
			if (templateIt != loadingCache.materialTemplateCache.end()) {
				desc = templateIt->second.desc;
			}
			ApplyBrniflyMaterialMetadata(desc, material.GetPrim());
		}
		if (mesh) {
			ApplyBrniflyMaterialMetadata(desc, mesh.GetPrim());
		}

		USDGeometryExtractor::ExtractOptions options{};
		options.brniflyVertexAlpha = desc.brniflyVertexAlpha;
		options.brniflyZBufferWrite = desc.brniflyZBufferWrite;
		options.brniflyDecal = desc.brniflyDecal;
		options.brniflyDynamicDecal = desc.brniflyDynamicDecal;
		options.brniflyModelSpaceNormals = desc.brniflyModelSpaceNormals;
		const bool temporaryBlockedOverlay = desc.brniflyDecal || desc.brniflyDynamicDecal;
		if (desc.brniflyVertexAlpha && desc.blendState == BlendState::BLEND_STATE_MASK && temporaryBlockedOverlay) {
			options.vertexAlphaCutoff = std::clamp(desc.alphaCutoff, 0.0f, 1.0f);
		}
		const bool atlasBakedHeightMode =
			stageOptions.objectReyesSurfaceSamplingMode == ObjectSurfaceSamplingMode::AtlasBakedHeight;
		const bool atlasBakedHeightNifListed =
			!atlasBakedHeightMode || ObjectReyesAtlasBakedHeightNifListed(stageOptions);
		const bool baseObjectReyesSelected =
			stageOptions.objectReyesNifMatched ||
			MaterialUsesWhitelistedTexture(desc, stageOptions.objectReyesTexturePaths);
		const bool objectReyesSelected =
			atlasBakedHeightMode ? (baseObjectReyesSelected && atlasBakedHeightNifListed) : baseObjectReyesSelected;
		const bool surfaceSamplingSelected =
			stageOptions.objectReyesSurfaceSamplingEnabled &&
			atlasBakedHeightNifListed &&
			((stageOptions.objectReyesSurfaceSamplingIncludeSelected && objectReyesSelected) ||
			 stageOptions.objectReyesSurfaceSamplingNifMatched ||
			 MaterialUsesWhitelistedTexture(desc, stageOptions.objectReyesSurfaceSamplingTexturePaths));
		const bool triplanarProjectionSelected =
			stageOptions.objectReyesTriplanarProjectionNifMatched ||
			MaterialUsesWhitelistedTexture(desc, stageOptions.objectReyesTriplanarProjectionTexturePaths) ||
			(stageOptions.objectReyesTriplanarProjectionIncludeSelected && (objectReyesSelected || surfaceSamplingSelected));
		const bool tripleTapStochasticSelected =
			stageOptions.objectReyesTripleTapStochasticNifMatched ||
			MaterialUsesWhitelistedTexture(desc, stageOptions.objectReyesTripleTapStochasticTexturePaths) ||
			(stageOptions.objectReyesTripleTapStochasticIncludeSelected &&
				(objectReyesSelected || surfaceSamplingSelected || triplanarProjectionSelected));
		const bool explicitHeightCandidate = SupportsObjectReyesGeometricDisplacementCandidate(desc);
		const bool potentialStaticHeightSidecar =
			(objectReyesSelected || surfaceSamplingSelected) &&
			!explicitHeightCandidate &&
			SupportsPotentialObjectReyesHeightSidecar(desc);
		const bool trustDeferredAtlasMaterialResolution =
			atlasBakedHeightMode &&
			surfaceSamplingSelected;
		options.geometricDisplacementOptIn = (objectReyesSelected || surfaceSamplingSelected) &&
			(trustDeferredAtlasMaterialResolution || explicitHeightCandidate || potentialStaticHeightSidecar);
		if (surfaceSamplingSelected && options.geometricDisplacementOptIn) {
			if (desc.brniflyModelSpaceNormals) {
				spdlog::warn(
					"Object Reyes surface sampling skipped for mesh '{}' material '{}' because model-space normal maps are not supported by v1.",
					mesh ? mesh.GetPrim().GetPath().GetString() : std::string("<null>"),
					material ? material.GetPrim().GetPath().GetString() : std::string("<unbound>"));
			}
			else {
				options.objectSurfaceSamplingMode = stageOptions.objectReyesSurfaceSamplingMode;
				options.objectSurfaceSamplingConfigHash = stageOptions.objectReyesConfigHash;
			}
		}
		if ((triplanarProjectionSelected || tripleTapStochasticSelected) && !desc.brniflyModelSpaceNormals) {
			options.objectSurfaceUseTriplanarProjection = triplanarProjectionSelected;
			options.objectSurfaceUseTripleTapStochastic = tripleTapStochasticSelected;
			options.objectSurfaceSamplingConfigHash = stageOptions.objectReyesConfigHash;
		}
		if ((objectReyesSelected || surfaceSamplingSelected) && !options.geometricDisplacementOptIn) {
			spdlog::info(
				"Object Reyes opt-in matched mesh '{}' material '{}' but material is not eligible for geometric Reyes/surface sampling: selected={}, surfaceSamplingSelected={}, baseSource='{}', geometric={}, heightTexture={}, heightSource='{}', baseAlphaHeight={}, firstHeightChannel={}, potentialStaticHeightSidecar={}.",
				mesh ? mesh.GetPrim().GetPath().GetString() : std::string("<null>"),
				material ? material.GetPrim().GetPath().GetString() : std::string("<unbound>"),
				objectReyesSelected,
				surfaceSamplingSelected,
				desc.baseColor.sourcePath,
				desc.enableGeometricDisplacement,
				desc.heightMap.texture != nullptr,
				desc.heightMap.sourcePath,
				desc.heightMapFromBaseColorAlpha,
				desc.heightMap.channels.empty() ? -1 : static_cast<int>(desc.heightMap.channels[0]),
				potentialStaticHeightSidecar);
		}
		return options;
	}

	bool ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(const USDGeometryExtractor::ExtractOptions& options)
	{
		return options.vertexAlphaCutoff.has_value() &&
			options.brniflyVertexAlpha &&
			(options.brniflyDecal || options.brniflyDynamicDecal);
	}

	std::string BuildTriplanarSubsetCombineKey(const MeshPreprocessWorkItem& workItem)
	{
		if (workItem.skinQ ||
			workItem.extractOptions.objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::TriplanarStochastic) {
			return {};
		}

		MaterialDescription desc{};
		if (workItem.material) {
			const std::string materialPath = workItem.material.GetPrim().GetPath().GetString();
			const auto templateIt = loadingCache.materialTemplateCache.find(materialPath);
			if (templateIt != loadingCache.materialTemplateCache.end()) {
				desc = templateIt->second.desc;
			}
			ApplyBrniflyMaterialMetadata(desc, workItem.material.GetPrim());
		}
		if (workItem.mesh) {
			ApplyBrniflyMaterialMetadata(desc, workItem.mesh.GetPrim());
		}

		spdlog::debug(
			"Object Reyes tri-planar combine candidate mesh='{}' material='{}' base='{}' normal='{}' roughness='{}' metallic='{}' ao='{}' height='{}' heightFromBaseAlpha={}.",
			workItem.mesh ? workItem.mesh.GetPrim().GetPath().GetString() : std::string("<null>"),
			workItem.material ? workItem.material.GetPrim().GetPath().GetString() : std::string("<unbound>"),
			desc.baseColor.sourcePath,
			desc.normal.sourcePath,
			desc.roughness.sourcePath,
			desc.metallic.sourcePath,
			desc.aoMap.sourcePath,
			desc.heightMap.sourcePath,
			desc.heightMapFromBaseColorAlpha);

		std::string key = BuildMaterialTextureSignature(desc);
		key += "optin=" + std::to_string(workItem.extractOptions.geometricDisplacementOptIn ? 1 : 0);
		key += "|surface=" + std::to_string(static_cast<std::uint32_t>(workItem.extractOptions.objectSurfaceSamplingMode));
		key += "|vtxalpha=" + std::to_string(workItem.extractOptions.brniflyVertexAlpha ? 1 : 0);
		key += "|zwrite=" + std::to_string(workItem.extractOptions.brniflyZBufferWrite ? 1 : 0);
		key += "|decal=" + std::to_string(workItem.extractOptions.brniflyDecal ? 1 : 0);
		key += "|dynamicDecal=" + std::to_string(workItem.extractOptions.brniflyDynamicDecal ? 1 : 0);
		key += "|modelNormals=" + std::to_string(workItem.extractOptions.brniflyModelSpaceNormals ? 1 : 0);
		key += "|alphaCutoff=";
		key += workItem.extractOptions.vertexAlphaCutoff
			? std::to_string(*workItem.extractOptions.vertexAlphaCutoff)
			: std::string("none");
		return key;
	}

	void AppendUniqueUvSetNames(std::vector<std::string>& dst, const std::vector<std::string>& src)
	{
		for (const std::string& name : src) {
			if (std::find(dst.begin(), dst.end(), name) == dst.end()) {
				dst.push_back(name);
			}
		}
	}

	std::vector<MeshPreprocessWorkItem> CombineCompatibleTriplanarSubsetWorkItems(
		std::vector<MeshPreprocessWorkItem>&& input)
	{
		std::vector<MeshPreprocessWorkItem> output;
		output.reserve(input.size());
		std::unordered_map<std::string, std::size_t> combinedByKey;
		for (MeshPreprocessWorkItem& item : input) {
			std::string key = BuildTriplanarSubsetCombineKey(item);
			if (key.empty()) {
				output.push_back(std::move(item));
				continue;
			}

			auto [it, inserted] = combinedByKey.emplace(key, output.size());
			if (inserted) {
				output.push_back(std::move(item));
				continue;
			}

			MeshPreprocessWorkItem& combined = output[it->second];
			combined.subsets.insert(
				combined.subsets.end(),
				std::make_move_iterator(item.subsets.begin()),
				std::make_move_iterator(item.subsets.end()));
			AppendUniqueUvSetNames(combined.requiredUvSetNames, item.requiredUvSetNames);
			combined.inferredDoubleSided = combined.inferredDoubleSided || item.inferredDoubleSided;
		}

		for (const MeshPreprocessWorkItem& item : output) {
			if (item.subsets.size() > 1u) {
				spdlog::info(
					"Object Reyes tri-planar preprocessing combined {} same-texture subset(s) for mesh '{}' material '{}'.",
					item.subsets.size(),
					item.meshPath,
					item.material ? item.material.GetPrim().GetPath().GetString() : std::string("<unbound>"));
			}
		}
		return output;
	}

	std::string ParentPrimPath(std::string path)
	{
		const std::size_t slash = path.find_last_of('/');
		if (slash == std::string::npos || slash == 0u) {
			return {};
		}
		return path.substr(0u, slash);
	}

	struct XMFLOAT3ExactKey
	{
		std::uint32_t x = 0;
		std::uint32_t y = 0;
		std::uint32_t z = 0;

		bool operator==(const XMFLOAT3ExactKey& other) const
		{
			return x == other.x && y == other.y && z == other.z;
		}
	};

	struct XMFLOAT3ExactKeyHash
	{
		std::size_t operator()(const XMFLOAT3ExactKey& key) const
		{
			std::size_t h = static_cast<std::size_t>(key.x);
			h ^= static_cast<std::size_t>(key.y) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
			h ^= static_cast<std::size_t>(key.z) + 0x9e3779b97f4a7c15ull + (h << 6u) + (h >> 2u);
			return h;
		}
	};

	XMFLOAT3ExactKey MakeExactPositionKey(const std::byte* vertex)
	{
		DirectX::XMFLOAT3 p{};
		std::memcpy(std::addressof(p), vertex + MeshVertexLayout::PositionOffset, sizeof(p));
		XMFLOAT3ExactKey key{};
		std::memcpy(std::addressof(key.x), std::addressof(p.x), sizeof(key.x));
		std::memcpy(std::addressof(key.y), std::addressof(p.y), sizeof(key.y));
		std::memcpy(std::addressof(key.z), std::addressof(p.z), sizeof(key.z));
		return key;
	}

	DirectX::XMFLOAT3 ReadPosition(const std::vector<std::byte>& vertices, unsigned int vertexSize, std::uint32_t index)
	{
		DirectX::XMFLOAT3 p{};
		std::memcpy(
			std::addressof(p),
			vertices.data() + static_cast<std::size_t>(index) * vertexSize + MeshVertexLayout::PositionOffset,
			sizeof(p));
		return p;
	}

	void AddNormal(DirectX::XMFLOAT3& dst, const DirectX::XMFLOAT3& n)
	{
		dst.x += n.x;
		dst.y += n.y;
		dst.z += n.z;
	}

	DirectX::XMFLOAT3 NormalizeOrFallback(const DirectX::XMFLOAT3& n)
	{
		const float lenSq = n.x * n.x + n.y * n.y + n.z * n.z;
		if (!std::isfinite(lenSq) || lenSq <= 1.0e-20f) {
			return { 0.0f, 0.0f, 1.0f };
		}
		const float invLen = 1.0f / std::sqrt(lenSq);
		return { n.x * invLen, n.y * invLen, n.z * invLen };
	}

	std::vector<std::string> BuildObjectReyesAtlasMaterialTextureSet(const std::vector<MaterialDescription>& materials)
	{
		std::vector<std::string> paths;
		paths.reserve(materials.size());
		for (const MaterialDescription& desc : materials) {
			const std::string normalized = NormalizeObjectReyesWhitelistPath(desc.baseColor.sourcePath);
			if (!normalized.empty() && std::find(paths.begin(), paths.end(), normalized) == paths.end()) {
				paths.push_back(normalized);
			}
		}
		std::sort(paths.begin(), paths.end());
		return paths;
	}

	bool ObjectReyesAtlasBakedMaterialCombinationAllowedForMaterials(
		const InMemoryStageOptions& stageOptions,
		const std::vector<MaterialDescription>& sourceMaterials,
		std::string_view parentPath)
	{
		if (stageOptions.objectReyesBakedHeightMaterials.empty()) {
			return true;
		}

		const std::vector<std::string> materialTextureSet =
			BuildObjectReyesAtlasMaterialTextureSet(sourceMaterials);
		for (const auto& entry : stageOptions.objectReyesBakedHeightMaterials) {
			if (entry.nifPath != stageOptions.objectReyesNifPath) {
				continue;
			}
			if (entry.materialTexturePaths.empty()) {
				return true;
			}
			if (entry.materialTexturePaths == materialTextureSet) {
				return true;
			}
		}

		std::string found;
		for (const std::string& path : materialTextureSet) {
			if (!found.empty()) {
				found += ", ";
			}
			found += path;
		}
		spdlog::warn(
			"Object Reyes atlas bake skipped under '{}': NIF '{}' material base texture set [{}] is not listed as a baked-height combination.",
			parentPath,
			stageOptions.objectReyesNifPath,
			found);
		return false;
	}

	bool ObjectReyesAtlasBakedMaterialCombinationAllowed(
		const InMemoryStageOptions& stageOptions,
		const MeshPreprocessResult& result,
		std::string_view parentPath)
	{
		return ObjectReyesAtlasBakedMaterialCombinationAllowedForMaterials(
			stageOptions,
			result.objectAtlasSourceMaterials,
			parentPath);
	}

	void RecomputeExactPositionWeldedNormals(
		std::vector<std::byte>& vertices,
		unsigned int vertexSize,
		unsigned int vertexFlags,
		const std::vector<std::uint32_t>& indices)
	{
		if (vertexSize == 0u || (vertexFlags & VertexFlags::VERTEX_NORMALS) == 0u) {
			return;
		}

		const std::size_t vertexCount = vertices.size() / static_cast<std::size_t>(vertexSize);
		std::unordered_map<XMFLOAT3ExactKey, std::uint32_t, XMFLOAT3ExactKeyHash> weldMap;
		weldMap.reserve(vertexCount);
		std::vector<std::uint32_t> vertexToWeld(vertexCount, 0u);
		std::vector<DirectX::XMFLOAT3> weldedNormals;
		for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
			const std::byte* vertex = vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize);
			auto [it, inserted] = weldMap.try_emplace(MakeExactPositionKey(vertex), static_cast<std::uint32_t>(weldedNormals.size()));
			if (inserted) {
				weldedNormals.push_back({});
			}
			vertexToWeld[vertexIndex] = it->second;
		}

		for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
			const std::uint32_t i0 = indices[i + 0u];
			const std::uint32_t i1 = indices[i + 1u];
			const std::uint32_t i2 = indices[i + 2u];
			if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
				continue;
			}
			const DirectX::XMFLOAT3 p0 = ReadPosition(vertices, vertexSize, i0);
			const DirectX::XMFLOAT3 p1 = ReadPosition(vertices, vertexSize, i1);
			const DirectX::XMFLOAT3 p2 = ReadPosition(vertices, vertexSize, i2);
			const DirectX::XMFLOAT3 e1{ p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
			const DirectX::XMFLOAT3 e2{ p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
			const DirectX::XMFLOAT3 n{
				e1.y * e2.z - e1.z * e2.y,
				e1.z * e2.x - e1.x * e2.z,
				e1.x * e2.y - e1.y * e2.x
			};
			AddNormal(weldedNormals[vertexToWeld[i0]], n);
			AddNormal(weldedNormals[vertexToWeld[i1]], n);
			AddNormal(weldedNormals[vertexToWeld[i2]], n);
		}

		for (DirectX::XMFLOAT3& n : weldedNormals) {
			n = NormalizeOrFallback(n);
		}
		for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex) {
			const DirectX::XMFLOAT3 n = weldedNormals[vertexToWeld[vertexIndex]];
			std::memcpy(
				vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize) + MeshVertexLayout::NormalOffset,
				std::addressof(n),
				sizeof(n));
		}
	}

	std::optional<MeshPreprocessResult> TryCombinePreprocessedMeshGroup(
		const std::vector<std::size_t>& group,
		const std::vector<MeshPreprocessWorkItem>& workItems,
		std::vector<std::optional<MeshPreprocessResult>>& preprocessed)
	{
		if (group.size() < 2u) {
			return std::nullopt;
		}

		MeshPreprocessResult& first = preprocessed[group.front()].value();
		const unsigned int vertexSize = first.ingest.GetVertexSize();
		const unsigned int skinningVertexSize = first.ingest.GetSkinningVertexSize();
		const unsigned int vertexFlags = first.ingest.GetFlags();
		if (skinningVertexSize != 0u) {
			return std::nullopt;
		}

		std::vector<std::byte> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<MeshUvSetData> uvSets = first.ingest.GetUvSets();
		std::size_t totalVertexCount = 0;
		std::size_t totalIndexCount = 0;
		for (std::size_t index : group) {
			const MeshPreprocessResult& result = preprocessed[index].value();
			if (result.ingest.GetVertexSize() != vertexSize ||
				result.ingest.GetSkinningVertexSize() != skinningVertexSize ||
				result.ingest.GetFlags() != vertexFlags ||
				result.ingest.GetUvSets().size() != uvSets.size()) {
				return std::nullopt;
			}
			totalVertexCount += result.ingest.GetVertices().size() / static_cast<std::size_t>(vertexSize);
			totalIndexCount += result.ingest.GetIndices().size();
		}
		vertices.reserve(totalVertexCount * static_cast<std::size_t>(vertexSize));
		indices.reserve(totalIndexCount);
		for (MeshUvSetData& uvSet : uvSets) {
			uvSet.values.clear();
			uvSet.values.reserve(totalVertexCount);
		}

		std::uint32_t vertexBase = 0u;
		for (std::size_t index : group) {
			const MeshPreprocessResult& result = preprocessed[index].value();
			const std::vector<std::byte>& srcVertices = result.ingest.GetVertices();
			const std::vector<std::uint32_t>& srcIndices = result.ingest.GetIndices();
			const std::vector<MeshUvSetData>& srcUvSets = result.ingest.GetUvSets();
			vertices.insert(vertices.end(), srcVertices.begin(), srcVertices.end());
			for (std::uint32_t srcIndex : srcIndices) {
				indices.push_back(vertexBase + srcIndex);
			}
			for (std::size_t uvSetIndex = 0; uvSetIndex < uvSets.size(); ++uvSetIndex) {
				uvSets[uvSetIndex].values.insert(
					uvSets[uvSetIndex].values.end(),
					srcUvSets[uvSetIndex].values.begin(),
					srcUvSets[uvSetIndex].values.end());
			}
			vertexBase += static_cast<std::uint32_t>(srcVertices.size() / static_cast<std::size_t>(vertexSize));
		}

		RecomputeExactPositionWeldedNormals(vertices, vertexSize, vertexFlags, indices);

		ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings(first.cacheIdentity.sourceIdentifier);
		builderSettings.doubleSidedVoxelSourceNormals = false;
		for (std::size_t index : group) {
			const MeshPreprocessWorkItem& item = workItems[index];
			builderSettings.doubleSidedVoxelSourceNormals =
				builderSettings.doubleSidedVoxelSourceNormals || item.authoredDoubleSided || item.inferredDoubleSided;
		}

		MeshIngestBuilder ingest(vertexSize, 0u, vertexFlags, builderSettings);
		ingest.SetUvSets(std::move(uvSets));
		ingest.ReserveVertices(totalVertexCount);
		for (std::size_t vertexIndex = 0; vertexIndex < totalVertexCount; ++vertexIndex) {
			ingest.AppendVertexBytes(vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize), vertexSize);
		}
		ingest.ReserveIndices(indices.size());
		ingest.AppendIndices(indices.data(), indices.size());

		CLodCacheLoader::MeshCacheIdentity identity = first.cacheIdentity;
		identity.subsetName = "combined-same-texture-meshes";
		identity.sourceIdentifier += "#combined_same_texture_meshes=" + std::to_string(group.size());
		for (std::size_t index : group) {
			identity.sourceIdentifier += "#combined_mesh=" + workItems[index].mesh.GetPrim().GetPath().GetString();
		}

		spdlog::info(
			"Object Reyes tri-planar preprocessing combining {} sibling mesh prim(s) under '{}' into one CLod mesh.",
			group.size(),
			ParentPrimPath(workItems[group.front()].mesh.GetPrim().GetPath().GetString()));
		ClusterLODPrebuildArtifacts artifacts = ingest.BuildClusterLODArtifacts();
		ClusterLODPrebuiltData savedPrebuiltData;
		std::optional<ClusterLODPrebuiltData> prebuiltData;
		if (CLodCacheLoader::SavePrebuiltLocked(identity, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload(), &savedPrebuiltData)) {
			prebuiltData = std::move(savedPrebuiltData);
		}
		else {
			spdlog::warn("Object Reyes tri-planar combined CLod cache save failed; using in-memory combined artifacts.");
			prebuiltData = std::move(artifacts.prebuiltData);
		}

		MeshPreprocessResult result(
			std::move(ingest),
			std::move(identity),
			std::move(prebuiltData),
			first.forceDoubleSidedPreview,
			std::move(first.prototypeGeometry));
		result.geometricDisplacementOptIn = first.geometricDisplacementOptIn;
		result.objectSurfaceSamplingMode = first.objectSurfaceSamplingMode;
		result.objectSurfaceUseTriplanarProjection = first.objectSurfaceUseTriplanarProjection;
		result.objectSurfaceUseTripleTapStochastic = first.objectSurfaceUseTripleTapStochastic;
		result.objectSurfaceTexelDensity = first.objectSurfaceTexelDensity;
		return result;
	}

	std::optional<std::vector<ObjectReyesAtlasBakedSubsetResult>> TryBuildObjectReyesAtlasBakedParentGroup(
		const std::vector<std::size_t>& group,
		const std::vector<MeshPreprocessWorkItem>& workItems,
		std::vector<std::optional<MeshPreprocessResult>>& preprocessed,
		const InMemoryStageOptions& stageOptions)
	{
		if (group.empty()) {
			return std::nullopt;
		}

		MeshPreprocessResult& first = preprocessed[group.front()].value();
		unsigned int vertexFlags = 0u;
		for (const std::size_t index : group) {
			const MeshPreprocessWorkItem& item = workItems[index];
			if (!preprocessed[index] ||
				!item.subsets.empty() ||
				item.skinQ ||
				item.extractOptions.objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::AtlasBakedHeight) {
				return std::nullopt;
			}
			const MeshPreprocessResult& result = preprocessed[index].value();
			if (result.ingest.GetSkinningVertexSize() != 0u) {
				spdlog::warn("Object Reyes atlas bake skipped: skinned meshes are not supported.");
				return std::nullopt;
			}
			vertexFlags |= result.ingest.GetFlags();
		}
		const unsigned int vertexSize = MeshVertexLayout::VertexSize(vertexFlags);
		const unsigned int skinningVertexSize = 0u;

		std::vector<br::import::ObjectReyesAtlasSourceMesh> sources;
		sources.reserve(group.size());
		std::vector<std::vector<std::byte>> normalizedSourceVertices(group.size());
		float texelsPerUnitSum = 0.0f;
		std::uint32_t texelsPerUnitCount = 0u;
		for (std::size_t groupEntry = 0; groupEntry < group.size(); ++groupEntry) {
			const std::size_t index = group[groupEntry];
			const MeshPreprocessWorkItem& item = workItems[index];
			const MeshPreprocessResult& result = preprocessed[index].value();
			const unsigned int sourceFlags = result.ingest.GetFlags();
			const unsigned int sourceVertexSize = result.ingest.GetVertexSize();
			const auto& sourceVertices = result.ingest.GetVertices();
			const std::size_t sourceVertexCount = sourceVertexSize != 0u
				? sourceVertices.size() / sourceVertexSize
				: 0u;
			auto& normalizedVertices = normalizedSourceVertices[groupEntry];
			normalizedVertices.assign(sourceVertexCount * static_cast<std::size_t>(vertexSize), std::byte{ 0 });
			for (std::size_t vertexIndex = 0; vertexIndex < sourceVertexCount; ++vertexIndex) {
				const std::byte* sourceVertex = sourceVertices.data() + vertexIndex * sourceVertexSize;
				std::byte* normalizedVertex = normalizedVertices.data() + vertexIndex * vertexSize;
				std::memcpy(normalizedVertex, sourceVertex, MeshVertexLayout::BaseVertexSize);
				if (MeshVertexLayout::HasTangents(vertexFlags)) {
					DirectX::XMFLOAT4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
					if (MeshVertexLayout::HasTangents(sourceFlags)) {
						std::memcpy(std::addressof(tangent), sourceVertex + MeshVertexLayout::TangentOffset(sourceFlags), sizeof(tangent));
					}
					std::memcpy(normalizedVertex + MeshVertexLayout::TangentOffset(vertexFlags), std::addressof(tangent), sizeof(tangent));
				}
				if (MeshVertexLayout::HasTexcoords(vertexFlags)) {
					DirectX::XMFLOAT2 texcoord{ 0.0f, 0.0f };
					if (MeshVertexLayout::HasTexcoords(sourceFlags)) {
						std::memcpy(std::addressof(texcoord), sourceVertex + MeshVertexLayout::TexcoordOffset(sourceFlags), sizeof(texcoord));
					}
					std::memcpy(normalizedVertex + MeshVertexLayout::TexcoordOffset(vertexFlags), std::addressof(texcoord), sizeof(texcoord));
				}
				if (MeshVertexLayout::HasColors(vertexFlags)) {
					DirectX::XMFLOAT3 color{ 1.0f, 1.0f, 1.0f };
					if (MeshVertexLayout::HasColors(sourceFlags)) {
						std::memcpy(std::addressof(color), sourceVertex + MeshVertexLayout::ColorOffset(sourceFlags), sizeof(color));
					}
					std::memcpy(normalizedVertex + MeshVertexLayout::ColorOffset(vertexFlags), std::addressof(color), sizeof(color));
				}
			}
			if (std::isfinite(result.objectSurfaceTexelDensity) && result.objectSurfaceTexelDensity > 0.0f) {
				texelsPerUnitSum += result.objectSurfaceTexelDensity;
				++texelsPerUnitCount;
			}
			sources.push_back(br::import::ObjectReyesAtlasSourceMesh{
				.vertices = std::addressof(normalizedVertices),
				.indices = std::addressof(result.ingest.GetIndices()),
				.uvSets = std::addressof(result.ingest.GetUvSets()),
				.vertexSize = vertexSize,
				.vertexFlags = vertexFlags,
				.materialIndex = static_cast<std::uint32_t>(groupEntry),
			});
		}

		br::import::ObjectReyesAtlasBakeOptions bakeOptions{};
		bakeOptions.resolution = std::clamp<std::uint32_t>(
			stageOptions.objectReyesAtlasBakeResolution,
			256u,
			8192u);
		bakeOptions.texelsPerUnit = bakeOptions.resolution > 0u
			? 0.0f
			: (texelsPerUnitCount > 0u ? texelsPerUnitSum / static_cast<float>(texelsPerUnitCount) : 1.0f);
		bakeOptions.maxAtlasSize = bakeOptions.resolution;
		bakeOptions.paddingTexels = std::min<std::uint32_t>(stageOptions.objectReyesAtlasBakePaddingTexels, 64u);

		const std::string parentPath = ParentPrimPath(workItems[group.front()].mesh.GetPrim().GetPath().GetString());
		br::import::ObjectReyesAtlasBakeResult atlasResult =
			br::import::BuildObjectReyesAtlasBakedHeightMesh(sources, bakeOptions, parentPath);
		if (!atlasResult.success) {
			spdlog::warn(
				"Object Reyes atlas bake failed under '{}': {}.",
				parentPath,
				atlasResult.error.empty() ? std::string("<unknown>") : atlasResult.error);
			return std::nullopt;
		}

		const std::size_t atlasVertexCount = atlasResult.vertices.size() / static_cast<std::size_t>(vertexSize);
		ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings(first.cacheIdentity.sourceIdentifier);
		builderSettings.doubleSidedVoxelSourceNormals = false;
		for (std::size_t index : group) {
			const MeshPreprocessWorkItem& item = workItems[index];
			builderSettings.doubleSidedVoxelSourceNormals =
				builderSettings.doubleSidedVoxelSourceNormals || item.authoredDoubleSided || item.inferredDoubleSided;
		}

		std::vector<std::string> sourceMaterialNames;
		std::vector<MaterialDescription> sourceMaterials;
		sourceMaterialNames.reserve(group.size());
		sourceMaterials.reserve(group.size());
		for (std::size_t index : group) {
			const MeshPreprocessWorkItem& item = workItems[index];
			sourceMaterialNames.push_back(item.mesh.GetPrim().GetName().GetString());
			MaterialDescription sourceDesc{};
			if (item.material) {
				const std::string materialPath = item.material.GetPrim().GetPath().GetString();
				if (const auto templateIt = loadingCache.materialTemplateCache.find(materialPath);
					templateIt != loadingCache.materialTemplateCache.end()) {
					sourceDesc = templateIt->second.desc;
				}
				ApplyBrniflyMaterialMetadata(sourceDesc, item.material.GetPrim());
			}
			if (item.mesh) {
				ApplyBrniflyMaterialMetadata(sourceDesc, item.mesh.GetPrim());
				sourceDesc.staticTextureOverrideSourceName = item.mesh.GetPrim().GetName().GetString();
			}
			sourceDesc.geometricDisplacementOptIn = true;
			sourceDesc.objectSurfaceSamplingMode = ObjectSurfaceSamplingMode::AtlasBakedHeight;
			if (index < preprocessed.size() && preprocessed[index]) {
				sourceDesc.objectSurfaceTexelDensity = preprocessed[index]->objectSurfaceTexelDensity;
				sourceDesc.objectSurfaceUseTriplanarProjection = preprocessed[index]->objectSurfaceUseTriplanarProjection;
				sourceDesc.objectSurfaceUseTripleTapStochastic = preprocessed[index]->objectSurfaceUseTripleTapStochastic;
			}
			sourceMaterials.push_back(std::move(sourceDesc));
		}
		if (!ObjectReyesAtlasBakedMaterialCombinationAllowedForMaterials(stageOptions, sourceMaterials, parentPath)) {
			return std::nullopt;
		}
		if (atlasResult.atlasUvSetIndex >= atlasResult.uvSets.size() ||
			atlasResult.uvSets[atlasResult.atlasUvSetIndex].values.size() != atlasVertexCount) {
			spdlog::warn(
				"Object Reyes atlas bake failed under '{}': generated atlas UV set {} is invalid for {} vertices.",
				parentPath,
				atlasResult.atlasUvSetIndex,
				atlasVertexCount);
			return std::nullopt;
		}

		auto sharedBakeData = std::make_shared<Mesh::ObjectReyesAtlasBakeData>();
		sharedBakeData->atlasWidth = atlasResult.atlasWidth;
		sharedBakeData->atlasHeight = atlasResult.atlasHeight;
		sharedBakeData->atlasUvSetIndex = atlasResult.atlasUvSetIndex;
		sharedBakeData->texelsPerUnit = atlasResult.texelsPerUnit;
		sharedBakeData->blendWidthObjectUnits = stageOptions.objectReyesBoundaryBlendStripWidthObjectUnits;
		sharedBakeData->storageFormat = stageOptions.objectReyesHeightAtlasStorage;
		sharedBakeData->atlasUvs = atlasResult.uvSets[atlasResult.atlasUvSetIndex].values;
		sharedBakeData->uvSets.clear();
		sharedBakeData->uvSets.reserve(atlasResult.uvSets.size());
		for (const MeshUvSetData& uvSet : atlasResult.uvSets) {
			sharedBakeData->uvSets.push_back(uvSet.values);
		}
		sharedBakeData->indices = atlasResult.indices;
		sharedBakeData->triangleMaterialIndices = atlasResult.triangleMaterialIndices;
		sharedBakeData->sourceMaterialNames = sourceMaterialNames;
		sharedBakeData->sourceMaterials = sourceMaterials;
		sharedBakeData->positions.resize(atlasVertexCount);
		sharedBakeData->normals.resize(atlasVertexCount, DirectX::XMFLOAT3{ 0.0f, 0.0f, 1.0f });
		const bool hasSharedNormals =
			(vertexFlags & VertexFlags::VERTEX_NORMALS) != 0u &&
			vertexSize >= MeshVertexLayout::NormalOffset + sizeof(DirectX::XMFLOAT3);
		for (std::size_t vertexIndex = 0; vertexIndex < atlasVertexCount; ++vertexIndex) {
			const std::byte* vertexBytes =
				atlasResult.vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize);
			std::memcpy(
				std::addressof(sharedBakeData->positions[vertexIndex]),
				vertexBytes + MeshVertexLayout::PositionOffset,
				sizeof(DirectX::XMFLOAT3));
			if (hasSharedNormals) {
				std::memcpy(
					std::addressof(sharedBakeData->normals[vertexIndex]),
					vertexBytes + MeshVertexLayout::NormalOffset,
					sizeof(DirectX::XMFLOAT3));
			}
		}

		auto buildPrototypeGeometry = [&](const std::vector<std::uint32_t>& subsetIndices) {
			br::import::RenderablePrototypeGeometry prototypeGeometry;
			prototypeGeometry.vertexFlags = vertexFlags;
			prototypeGeometry.indices.assign(subsetIndices.begin(), subsetIndices.end());
			if (vertexSize != 0u) {
				prototypeGeometry.vertices.reserve(atlasVertexCount);
				const bool hasNormals =
					(vertexFlags & VertexFlags::VERTEX_NORMALS) != 0u &&
					vertexSize >= MeshVertexLayout::NormalOffset + sizeof(DirectX::XMFLOAT3);
				const bool hasTexcoords =
					(vertexFlags & VertexFlags::VERTEX_TEXCOORDS) != 0u &&
					vertexSize >= MeshVertexLayout::TexcoordOffset(vertexFlags) + sizeof(DirectX::XMFLOAT2);
				const bool hasTangents =
					(vertexFlags & VertexFlags::VERTEX_TANGENTS) != 0u &&
					vertexSize >= MeshVertexLayout::TangentOffset(vertexFlags) + sizeof(DirectX::XMFLOAT4);
				const bool hasColors =
					(vertexFlags & VertexFlags::VERTEX_COLORS) != 0u &&
					vertexSize >= MeshVertexLayout::ColorOffset(vertexFlags) + sizeof(DirectX::XMFLOAT3);
				for (std::size_t vertexIndex = 0; vertexIndex < atlasVertexCount; ++vertexIndex) {
					const std::byte* vertexBytes =
						atlasResult.vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize);
					br::import::RenderablePrototypeVertex vertex{};
					std::memcpy(std::addressof(vertex.position), vertexBytes + MeshVertexLayout::PositionOffset, sizeof(vertex.position));
					if (hasNormals) {
						std::memcpy(std::addressof(vertex.normal), vertexBytes + MeshVertexLayout::NormalOffset, sizeof(vertex.normal));
					}
					if (hasTexcoords) {
						std::memcpy(std::addressof(vertex.uv), vertexBytes + MeshVertexLayout::TexcoordOffset(vertexFlags), sizeof(vertex.uv));
					}
					if (hasTangents) {
						std::memcpy(std::addressof(vertex.tangent), vertexBytes + MeshVertexLayout::TangentOffset(vertexFlags), sizeof(vertex.tangent));
					}
					if (hasColors) {
						DirectX::XMFLOAT3 color{};
						std::memcpy(std::addressof(color), vertexBytes + MeshVertexLayout::ColorOffset(vertexFlags), sizeof(color));
						vertex.color = DirectX::XMFLOAT4{ color.x, color.y, color.z, 1.0f };
					}
					prototypeGeometry.vertices.push_back(vertex);
				}
			}
			return prototypeGeometry;
		};

		spdlog::info(
			"Object Reyes atlas-baked height preprocessing combined {} sibling mesh prim(s) under '{}' into one shared atlas ({}x{}, uvSet={}, density={}) and will emit {} material subset mesh(es).",
			group.size(),
			parentPath,
			atlasResult.atlasWidth,
			atlasResult.atlasHeight,
			atlasResult.atlasUvSetIndex,
			atlasResult.texelsPerUnit,
			group.size());
		if (!atlasResult.diagnostics.empty()) {
			spdlog::info(
				"Object Reyes atlas-baked height diagnostics under '{}': {}.",
				parentPath,
				atlasResult.diagnostics);
		}

		std::vector<std::vector<std::uint32_t>> indicesByMaterial(group.size());
		const std::size_t triangleCount = atlasResult.indices.size() / 3u;
		for (std::size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex) {
			const std::uint32_t materialIndex = triangleIndex < atlasResult.triangleMaterialIndices.size()
				? atlasResult.triangleMaterialIndices[triangleIndex]
				: 0u;
			if (materialIndex >= indicesByMaterial.size()) {
				spdlog::warn(
					"Object Reyes atlas bake failed under '{}': generated triangle {} references invalid material index {}.",
					parentPath,
					triangleIndex,
					materialIndex);
				return std::nullopt;
			}
			std::vector<std::uint32_t>& subsetIndices = indicesByMaterial[materialIndex];
			subsetIndices.push_back(atlasResult.indices[triangleIndex * 3u + 0u]);
			subsetIndices.push_back(atlasResult.indices[triangleIndex * 3u + 1u]);
			subsetIndices.push_back(atlasResult.indices[triangleIndex * 3u + 2u]);
		}

		std::vector<ObjectReyesAtlasBakedSubsetResult> subsetResults;
		subsetResults.reserve(group.size());
		for (std::size_t groupEntry = 0; groupEntry < group.size(); ++groupEntry) {
			std::vector<std::uint32_t>& subsetIndices = indicesByMaterial[groupEntry];
			if (subsetIndices.empty()) {
				spdlog::warn(
					"Object Reyes atlas bake failed under '{}': source mesh '{}' produced no atlas triangles.",
					parentPath,
					workItems[group[groupEntry]].mesh.GetPrim().GetPath().GetString());
				return std::nullopt;
			}

			MeshIngestBuilder ingest(vertexSize, 0u, vertexFlags, builderSettings);
			ingest.SetUvSets(std::vector<MeshUvSetData>(atlasResult.uvSets));
			ingest.ReserveVertices(atlasVertexCount);
			for (std::size_t vertexIndex = 0; vertexIndex < atlasVertexCount; ++vertexIndex) {
				ingest.AppendVertexBytes(
					atlasResult.vertices.data() + vertexIndex * static_cast<std::size_t>(vertexSize),
					vertexSize);
			}
			ingest.ReserveIndices(subsetIndices.size());
			ingest.AppendIndices(subsetIndices.data(), subsetIndices.size());

			const std::size_t workIndex = group[groupEntry];
			const MeshPreprocessWorkItem& item = workItems[workIndex];
			CLodCacheLoader::MeshCacheIdentity identity = preprocessed[workIndex]->cacheIdentity;
			identity.subsetName = "object-reyes-atlas-baked-height-" + item.mesh.GetPrim().GetName().GetString();
			identity.sourceIdentifier += "#object_reyes_atlas_baked_height_version=18";
			identity.sourceIdentifier += "#object_reyes_atlas_parent=" + parentPath;
			identity.sourceIdentifier += "#object_reyes_atlas_render_material=" + std::to_string(groupEntry);
			identity.sourceIdentifier += "#object_reyes_atlas_uv=" + std::to_string(atlasResult.atlasUvSetIndex);
			identity.sourceIdentifier += "#object_reyes_atlas_size=" +
				std::to_string(atlasResult.atlasWidth) + "x" + std::to_string(atlasResult.atlasHeight);
			identity.sourceIdentifier += "#object_reyes_atlas_requested_resolution=" + std::to_string(bakeOptions.resolution);
			identity.sourceIdentifier += "#object_reyes_atlas_padding=" + std::to_string(bakeOptions.paddingTexels);
			identity.sourceIdentifier += "#object_reyes_atlas_blend_width=" + std::to_string(stageOptions.objectReyesBoundaryBlendStripWidthObjectUnits);
			for (std::size_t sourceIndex : group) {
				identity.sourceIdentifier += "#atlas_source_mesh=" + workItems[sourceIndex].mesh.GetPrim().GetPath().GetString();
			}

			ClusterLODPrebuildArtifacts artifacts = ingest.BuildClusterLODArtifacts();
			bool atlasUvPresentInEveryPage =
				artifacts.prebuiltData.trianglePageCount != 0u &&
				artifacts.prebuiltData.trianglePageCount <= artifacts.cacheBuildData.meshPageBlobs.size();
			for (std::uint32_t pageIndex = 0u;
				atlasUvPresentInEveryPage && pageIndex < artifacts.prebuiltData.trianglePageCount;
				++pageIndex) {
				const auto& page = artifacts.cacheBuildData.meshPageBlobs[pageIndex];
				if (page.size() < sizeof(CLodPageHeader)) {
					atlasUvPresentInEveryPage = false;
					break;
				}
				CLodPageHeader header{};
				std::memcpy(std::addressof(header), page.data(), sizeof(header));
				atlasUvPresentInEveryPage =
					header.formatAndKind == CLOD_TRIANGLE_PAGE_MAGIC &&
					atlasResult.atlasUvSetIndex < header.uvSetCount;
			}
			if (!atlasUvPresentInEveryPage) {
				spdlog::error(
					"Object Reyes atlas-baked CLod build under '{}' lost required UV set {} before cache publication.",
					parentPath,
					atlasResult.atlasUvSetIndex);
				return std::nullopt;
			}
			ClusterLODPrebuiltData savedPrebuiltData;
			std::optional<ClusterLODPrebuiltData> prebuiltData;
			if (CLodCacheLoader::SavePrebuiltLocked(identity, artifacts.prebuiltData, artifacts.cacheBuildData.AsPayload(), &savedPrebuiltData)) {
				prebuiltData = std::move(savedPrebuiltData);
			}
			else {
				spdlog::warn(
					"Object Reyes atlas-baked CLod cache save failed for subset '{}' under '{}'; using in-memory artifacts.",
					item.mesh.GetPrim().GetName().GetString(),
					parentPath);
				prebuiltData = std::move(artifacts.prebuiltData);
			}

			MeshPreprocessResult result(
				std::move(ingest),
				std::move(identity),
				std::move(prebuiltData),
				preprocessed[workIndex]->forceDoubleSidedPreview,
				buildPrototypeGeometry(subsetIndices));
			result.geometricDisplacementOptIn = true;
			result.objectSurfaceSamplingMode = ObjectSurfaceSamplingMode::AtlasBakedHeight;
			result.objectSurfaceUseTriplanarProjection = preprocessed[workIndex]->objectSurfaceUseTriplanarProjection;
			result.objectSurfaceUseTripleTapStochastic = preprocessed[workIndex]->objectSurfaceUseTripleTapStochastic;
			result.objectSurfaceTexelDensity = preprocessed[workIndex]->objectSurfaceTexelDensity;
			result.objectAtlasBakedHeight = true;
			result.objectAtlasHeightUvSetIndex = atlasResult.atlasUvSetIndex;
			result.objectAtlasWidth = atlasResult.atlasWidth;
			result.objectAtlasHeight = atlasResult.atlasHeight;
			result.objectAtlasTexelsPerUnit = atlasResult.texelsPerUnit;
			result.objectAtlasBlendWidthObjectUnits = stageOptions.objectReyesBoundaryBlendStripWidthObjectUnits;
			result.objectAtlasTriangleMaterialIndices.assign(subsetIndices.size() / 3u, static_cast<std::uint32_t>(groupEntry));
			result.objectAtlasSourceMaterialNames = sourceMaterialNames;
			result.objectAtlasSourceMaterials = sourceMaterials;
			result.objectAtlasSharedBakeData = sharedBakeData;
			subsetResults.push_back(ObjectReyesAtlasBakedSubsetResult{
				.sourceWorkIndex = workIndex,
				.result = std::move(result),
			});
		}

		return subsetResults;
	}

	void DisableObjectReyesForGroup(
		const std::vector<std::size_t>& group,
		std::vector<std::optional<MeshPreprocessResult>>& preprocessed)
	{
		for (std::size_t index : group) {
			if (!preprocessed[index]) {
				continue;
			}
			preprocessed[index]->geometricDisplacementOptIn = false;
			preprocessed[index]->objectSurfaceSamplingMode = ObjectSurfaceSamplingMode::None;
			preprocessed[index]->objectSurfaceUseTriplanarProjection = false;
			preprocessed[index]->objectSurfaceUseTripleTapStochastic = false;
			preprocessed[index]->objectSurfaceTexelDensity = 1.0f;
		}
	}

	std::optional<CLodCacheLoader::MeshCacheIdentity> BuildPointInstancerAssemblyIdentity(
		const UsdStageRefPtr& stage,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode);

	void TryLoadPointInstancerAssemblyMesh(
		const UsdStageRefPtr& stage,
		UsdTimeCode geomTimeCode,
		const std::string& sourceIdentifier);

	void PreprocessAllMeshes(
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		const std::string& directory,
		bool isUSDZ,
		const ImportSettings& importSettings,
		const InMemoryStageOptions& stageOptions,
		const std::string& sourceIdentifierOverride = {},
		bool retainArtifactsForAssetAssembly = false)
	{
		ZoneScopedN("USDLoader::PreprocessAllMeshes");
		ZoneText(sourceIdentifierOverride.data(), sourceIdentifierOverride.size());
		loadingCache.preprocessedMeshCache.clear();
		loadingCache.skippedPreprocessedMeshReasons.clear();

		const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);
		UsdSkelCache preprocessSkelCache;
		std::vector<MeshPreprocessWorkItem> workItems;
		std::unordered_set<std::string> queuedWorkItemKeys;
		auto markSkippedMesh = [](const std::string& meshPath, std::string reason) {
			if (!meshPath.empty()) {
				loadingCache.skippedPreprocessedMeshReasons.emplace(meshPath, std::move(reason));
			}
		};
		auto enqueueWorkItem = [&](MeshPreprocessWorkItem&& item) {
			std::ostringstream key;
			key << item.meshPath << '|';
			if (item.subsets.empty()) {
				key << "<mesh>";
			}
			else {
				for (const auto& subset : item.subsets) {
					key << subset.GetPrim().GetPath().GetString() << ';';
				}
			}
			key << '|'
				<< (item.material ? item.material.GetPrim().GetPath().GetString() : std::string("<unbound>")) << '|'
				<< static_cast<int>(item.authoredDoubleSided) << '|'
				<< static_cast<int>(item.inferredDoubleSided) << '|'
				<< static_cast<int>(item.skinQ.has_value()) << '|'
				<< static_cast<int>(item.extractOptions.retainClusterLODArtifacts) << '|'
				<< static_cast<int>(item.extractOptions.importSkinningAsRigidBindPose) << '|'
				<< static_cast<int>(item.extractOptions.objectSurfaceSamplingMode) << '|'
				<< item.extractOptions.objectSurfaceSamplingConfigHash << '|'
				<< static_cast<int>(item.extractOptions.objectSurfaceUseTriplanarProjection) << '|'
				<< static_cast<int>(item.extractOptions.objectSurfaceUseTripleTapStochastic) << '|'
				<< static_cast<int>(item.extractOptions.brniflyVertexAlpha) << '|'
				<< static_cast<int>(item.extractOptions.brniflyZBufferWrite) << '|'
				<< static_cast<int>(item.extractOptions.brniflyDecal) << '|'
				<< static_cast<int>(item.extractOptions.brniflyDynamicDecal) << '|'
				<< static_cast<int>(item.extractOptions.brniflyModelSpaceNormals);
			if (!queuedWorkItemKeys.insert(key.str()).second) {
				return;
			}
			workItems.push_back(std::move(item));
		};

		std::function<void(const UsdPrim&)> gatherMeshJobs = [&](const UsdPrim& prim) {
			if (prim.IsA<UsdGeomImageable>()) {
				UsdGeomImageable imageable(prim);
				if (imageable.ComputeVisibility(geomTimeCode) == UsdGeomTokens->invisible) {
					return;
				}
			}

			UsdGeomMesh mesh(prim);
			if (mesh) {
				const std::string meshPath = mesh.GetPrim().GetPath().GetString();
				if (IsBrNiflyCollisionMesh(mesh)) {
					spdlog::info("Skipping BRNifly collision mesh '{}'.", meshPath);
					markSkippedMesh(meshPath, "collision mesh");
					return;
				}

				if (!retainArtifactsForAssetAssembly && IsUnsupportedBrNiflySkinnedMesh(mesh)) {
					spdlog::info(
						"Skipping BRNifly skinned mesh '{}' until NIF skeleton pose updates are supported.",
						meshPath);
					markSkippedMesh(meshPath, "unsupported skinned mesh");
					return;
				}

				auto skinQ = USDGeometryExtractor::GetSkinningQuery(mesh, preprocessSkelCache);
				VtTokenArray skelJointOrderRaw;
				VtTokenArray skelJointOrderMapped;
				if (skinQ) {
					UsdSkelBindingAPI bindAPI(mesh.GetPrim());
					UsdSkelSkeleton skel = bindAPI.GetInheritedSkeleton();
					if (skel) {
						if (const UsdSkelRoot skelRoot = UsdSkelRoot::Find(skel.GetPrim()))
							preprocessSkelCache.Populate(skelRoot, UsdPrimDefaultPredicate);
						auto skelQuery = preprocessSkelCache.GetSkelQuery(skel);
						skelJointOrderRaw = skelQuery.GetJointOrder();

						auto& mapper = skinQ->GetJointMapper();
						if (mapper && !mapper->IsIdentity()) {
							mapper->Remap(skelJointOrderRaw, &skelJointOrderMapped);
						}
						else {
							skelJointOrderMapped = skelJointOrderRaw;
						}
					}
				}

				bool authoredDoubleSided = false;
				UsdGeomGprim gprim(mesh.GetPrim());
				if (gprim) {
					gprim.GetDoubleSidedAttr().Get(&authoredDoubleSided, geomTimeCode);
				}

				UsdShadeMaterialBindingAPI bindAPI(mesh);
				auto subsets = bindAPI.GetMaterialBindSubsets();

				const auto getRequiredUvSetNames = [](const UsdShadeMaterial& material) {
					if (!material) {
						return std::vector<std::string>{};
					}
					const auto templateIt = loadingCache.materialTemplateCache.find(material.GetPrim().GetPath().GetString());
					return templateIt != loadingCache.materialTemplateCache.end()
						? templateIt->second.referencedUvSetNames
						: std::vector<std::string>{};
				};

				if (subsets.empty()) {
					auto mat = UsdShadeMaterialBindingAPI(mesh).ComputeBoundMaterial();
					ProcessMaterial(mat, stage, stageOptions, isUSDZ, directory, importSettings.loadMaterialTextures);
					auto extractOptions = BuildGeometryExtractOptions(mesh, mat, stageOptions);
					extractOptions.retainClusterLODArtifacts = extractOptions.retainClusterLODArtifacts ||
						retainArtifactsForAssetAssembly ||
						(importSettings.prepareObjectReyesAtlasRecipes && stageOptions.objectReyesNifMatched);
					extractOptions.skipCachedClusterLODMeshBuilds = !extractOptions.retainClusterLODArtifacts &&
						!(importSettings.prepareObjectReyesAtlasRecipes && stageOptions.objectReyesNifMatched);
					if (ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(extractOptions)) {
						spdlog::info(
							"Temporarily skipping BRNifly vertex-alpha overlay mesh '{}' material '{}' (zwrite={}, decal={}, dynamicDecal={}, cutoff={}).",
							meshPath,
							mat ? mat.GetPrim().GetPath().GetString() : std::string("<unbound>"),
							extractOptions.brniflyZBufferWrite,
							extractOptions.brniflyDecal,
							extractOptions.brniflyDynamicDecal,
							extractOptions.vertexAlphaCutoff.value());
						markSkippedMesh(meshPath, "temporary BRNifly vertex-alpha overlay block");
						return;
					}
					const bool inferredDoubleSided = ShouldForceDoubleSidedByName(mat, std::nullopt, importSettings);
					enqueueWorkItem(MeshPreprocessWorkItem{
						.meshPath = meshPath,
						.mesh = mesh,
						.subsets = {},
						.material = mat,
						.requiredUvSetNames = getRequiredUvSetNames(mat),
						.skinQ = skinQ,
						.skelJointOrderRaw = skelJointOrderRaw,
						.skelJointOrderMapped = skelJointOrderMapped,
						.extractOptions = extractOptions,
						.authoredDoubleSided = authoredDoubleSided,
						.inferredDoubleSided = inferredDoubleSided
						});
				}
				else {
					std::vector<MeshPreprocessWorkItem> subsetWorkItems;
					subsetWorkItems.reserve(subsets.size());
					for (const auto& subset : subsets) {
						auto mat = UsdShadeMaterialBindingAPI(subset).ComputeBoundMaterial();
						ProcessMaterial(mat, stage, stageOptions, isUSDZ, directory, importSettings.loadMaterialTextures);
						auto extractOptions = BuildGeometryExtractOptions(mesh, mat, stageOptions);
						extractOptions.retainClusterLODArtifacts = extractOptions.retainClusterLODArtifacts ||
							retainArtifactsForAssetAssembly ||
							(importSettings.prepareObjectReyesAtlasRecipes && stageOptions.objectReyesNifMatched);
						extractOptions.skipCachedClusterLODMeshBuilds = !extractOptions.retainClusterLODArtifacts &&
							!(importSettings.prepareObjectReyesAtlasRecipes && stageOptions.objectReyesNifMatched);
						if (ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(extractOptions)) {
							spdlog::info(
								"Temporarily skipping BRNifly vertex-alpha overlay mesh '{}' subset '{}' material '{}' (zwrite={}, decal={}, dynamicDecal={}, cutoff={}).",
								meshPath,
								subset.GetPrim().GetName().GetString(),
								mat ? mat.GetPrim().GetPath().GetString() : std::string("<unbound>"),
								extractOptions.brniflyZBufferWrite,
								extractOptions.brniflyDecal,
								extractOptions.brniflyDynamicDecal,
								extractOptions.vertexAlphaCutoff.value());
							continue;
						}
						const bool inferredDoubleSided = ShouldForceDoubleSidedByName(mat, subset, importSettings);
						subsetWorkItems.push_back(MeshPreprocessWorkItem{
							.meshPath = meshPath,
							.mesh = mesh,
							.subsets = { subset },
							.material = mat,
							.requiredUvSetNames = getRequiredUvSetNames(mat),
							.skinQ = skinQ,
							.skelJointOrderRaw = skelJointOrderRaw,
							.skelJointOrderMapped = skelJointOrderMapped,
							.extractOptions = extractOptions,
							.authoredDoubleSided = authoredDoubleSided,
							.inferredDoubleSided = inferredDoubleSided
							});
						if (inferredDoubleSided) {
							spdlog::info("USD double-sided heuristic enabled for mesh '{}' subset '{}' material '{}'",
								meshPath,
								subset.GetPrim().GetName().GetString(),
								mat ? mat.GetPrim().GetName().GetString() : std::string("<unbound>"));
						}
					}
					if (subsetWorkItems.empty()) {
						markSkippedMesh(meshPath, "all subsets temporarily skipped");
					}
					else {
						auto combinedSubsetWorkItems = CombineCompatibleTriplanarSubsetWorkItems(std::move(subsetWorkItems));
						for (MeshPreprocessWorkItem& item : combinedSubsetWorkItems) {
							enqueueWorkItem(std::move(item));
						}
					}
				}
			}

			for (auto child : prim.GetFilteredChildren(UsdTraverseInstanceProxies())) {
				gatherMeshJobs(child);
			}
		};
		{
			ZoneScopedN("USDLoader::PreprocessAllMeshes::GatherMeshJobs");
			gatherMeshJobs(stage->GetPseudoRoot());
		}

		spdlog::debug("USD mesh preprocessing: gathered {} mesh/subset job(s).", workItems.size());
		TracyPlot("SARP.Import.USD.Preprocess.WorkItems", static_cast<int64_t>(workItems.size()));
		std::vector<std::optional<MeshPreprocessResult>> preprocessed(workItems.size());
		TaskSchedulerManager::GetInstance().ParallelFor("USDLoader::PreprocessMeshes", workItems.size(), [&](size_t workIndex) {
			ZoneScopedN("USDLoader::PreprocessAllMeshes::ExtractSubMesh");
			const MeshPreprocessWorkItem& workItem = workItems[workIndex];
			ZoneText(workItem.meshPath.data(), workItem.meshPath.size());
			preprocessed[workIndex] = USDGeometryExtractor::ExtractSubMeshGroup(
				workItem.mesh,
				workItem.subsets,
				stage,
				geomTimeCode,
				metersPerUnit,
				workItem.requiredUvSetNames,
				workItem.skinQ,
				workItem.skelJointOrderRaw,
				workItem.skelJointOrderMapped,
				workItem.authoredDoubleSided || workItem.inferredDoubleSided,
				sourceIdentifierOverride,
				importSettings.nifTessellationFactor,
				workItem.extractOptions);
			});

		{
			ZoneScopedN("USDLoader::PreprocessAllMeshes::PublishPreprocessedResults");
			std::vector<bool> consumed(workItems.size(), false);
			std::unordered_map<std::string, std::vector<std::size_t>> atlasBakeGroups;
			for (std::size_t workIndex = 0; workIndex < workItems.size(); ++workIndex) {
				if (!preprocessed[workIndex].has_value()) {
					throw std::runtime_error("Missing preprocessed USD mesh data");
				}
				const MeshPreprocessWorkItem& workItem = workItems[workIndex];
				if (!workItem.subsets.empty() ||
					workItem.skinQ ||
					workItem.extractOptions.objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::AtlasBakedHeight) {
					continue;
				}
				const std::string parentPath = ParentPrimPath(workItem.mesh.GetPrim().GetPath().GetString());
				atlasBakeGroups[parentPath].push_back(workIndex);
			}
			for (const auto& [parentPath, group] : atlasBakeGroups) {
				std::vector<std::size_t> orderedGroup = group;
				std::ranges::stable_sort(orderedGroup, [&](std::size_t lhs, std::size_t rhs) {
					const std::string lhsPath = lhs < workItems.size() && workItems[lhs].mesh
						? workItems[lhs].mesh.GetPrim().GetPath().GetString()
						: std::string{};
					const std::string rhsPath = rhs < workItems.size() && workItems[rhs].mesh
						? workItems[rhs].mesh.GetPrim().GetPath().GetString()
						: std::string{};
					return lhsPath < rhsPath;
				});
				if (orderedGroup.empty()) {
					continue;
				}
				if (!importSettings.prepareObjectReyesAtlasRecipes) {
					DisableObjectReyesForGroup(orderedGroup, preprocessed);
					spdlog::warn(
						"Object Reyes atlas manifest unavailable for cold runtime import under '{}'; using clean opaque fallback without source-geometry inspection.",
						parentPath);
					continue;
				}
				auto bakedSubsets = TryBuildObjectReyesAtlasBakedParentGroup(orderedGroup, workItems, preprocessed, stageOptions);
				if (!bakedSubsets) {
					spdlog::warn(
						"Object Reyes atlas-baked height failed under '{}'; disabling Object Reyes geometric displacement for that parent.",
						parentPath);
					DisableObjectReyesForGroup(orderedGroup, preprocessed);
					continue;
				}

				const std::size_t ownerIndex = orderedGroup.front();
				const MeshPreprocessWorkItem& ownerItem = workItems[ownerIndex];
				auto& ownerRecord = loadingCache.preprocessedMeshCache[ownerItem.meshPath];
				ownerRecord.authoredDoubleSided = ownerItem.authoredDoubleSided;
				for (ObjectReyesAtlasBakedSubsetResult& bakedSubset : *bakedSubsets) {
					if (bakedSubset.sourceWorkIndex >= workItems.size()) {
						continue;
					}
					const MeshPreprocessWorkItem& sourceItem = workItems[bakedSubset.sourceWorkIndex];
					ownerRecord.subsets.emplace_back(
						sourceItem.material,
						std::move(bakedSubset.result),
						sourceItem.inferredDoubleSided,
						sourceItem.mesh.GetPrim().GetName().GetString());
				}

				for (std::size_t groupEntry = 0u; groupEntry < orderedGroup.size(); ++groupEntry) {
					const std::size_t index = orderedGroup[groupEntry];
					const MeshPreprocessWorkItem& item = workItems[index];
					if (index != ownerIndex) {
						auto& emptyRecord = loadingCache.preprocessedMeshCache[item.meshPath];
						emptyRecord.authoredDoubleSided = item.authoredDoubleSided;
					}
					consumed[index] = true;
				}
			}

			std::unordered_map<std::string, std::vector<std::size_t>> meshCombineGroups;
			for (std::size_t workIndex = 0; workIndex < workItems.size(); ++workIndex) {
				if (consumed[workIndex]) {
					continue;
				}
				if (!preprocessed[workIndex].has_value()) {
					throw std::runtime_error("Missing preprocessed USD mesh data");
				}
				const MeshPreprocessWorkItem& workItem = workItems[workIndex];
				if (!workItem.subsets.empty()) {
					continue;
				}
				const std::string combineKey = BuildTriplanarSubsetCombineKey(workItem);
				if (combineKey.empty()) {
					continue;
				}
				const std::string parentPath = ParentPrimPath(workItem.mesh.GetPrim().GetPath().GetString());
				meshCombineGroups[parentPath + "|" + combineKey].push_back(workIndex);
			}
			for (const auto& [_, group] : meshCombineGroups) {
				if (group.size() < 2u) {
					continue;
				}
				auto combined = TryCombinePreprocessedMeshGroup(group, workItems, preprocessed);
				if (!combined) {
					continue;
				}

				const std::size_t ownerIndex = group.front();
				const MeshPreprocessWorkItem& ownerItem = workItems[ownerIndex];
				auto& ownerRecord = loadingCache.preprocessedMeshCache[ownerItem.meshPath];
				ownerRecord.authoredDoubleSided = ownerItem.authoredDoubleSided;
				ownerRecord.subsets.emplace_back(
					ownerItem.material,
					std::move(*combined),
					ownerItem.inferredDoubleSided,
					ownerItem.mesh.GetPrim().GetName().GetString());
				consumed[ownerIndex] = true;

				for (std::size_t groupEntry = 1u; groupEntry < group.size(); ++groupEntry) {
					const std::size_t index = group[groupEntry];
					const MeshPreprocessWorkItem& item = workItems[index];
					auto& emptyRecord = loadingCache.preprocessedMeshCache[item.meshPath];
					emptyRecord.authoredDoubleSided = item.authoredDoubleSided;
					consumed[index] = true;
				}
			}

			for (size_t workIndex = 0; workIndex < workItems.size(); ++workIndex) {
				if (consumed[workIndex]) {
					continue;
				}
				if (!preprocessed[workIndex].has_value()) {
					throw std::runtime_error("Missing preprocessed USD mesh data");
				}

				const MeshPreprocessWorkItem& workItem = workItems[workIndex];
				auto& record = loadingCache.preprocessedMeshCache[workItem.meshPath];
				record.authoredDoubleSided = workItem.authoredDoubleSided;
				record.subsets.emplace_back(
					workItem.material,
					std::move(preprocessed[workIndex].value()),
					workItem.inferredDoubleSided,
					workItem.mesh.GetPrim().GetName().GetString());
			}
		}

		TryLoadPointInstancerAssemblyMesh(stage, geomTimeCode, sourceIdentifierOverride);
	}

	std::optional<CLodCacheLoader::MeshCacheIdentity> BuildPointInstancerAssemblyIdentity(
		const UsdStageRefPtr& stage,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode)
	{
		if (!stage) {
			return std::nullopt;
		}

		auto pathHasAnyPrefix = [](const SdfPath& path, const std::vector<SdfPath>& prefixes) {
			for (const SdfPath& prefix : prefixes) {
				if (path.HasPrefix(prefix)) {
					return true;
				}
			}
			return false;
		};

		std::vector<SdfPath> prototypeRoots;
		std::set<std::string> prototypeRootStrings;
		bool hasPointInstancer = false;
		auto instancerRange = UsdPrimRange(stage->GetPseudoRoot());
		for (auto primIt = instancerRange.begin(); primIt != instancerRange.end(); ++primIt) {
			UsdGeomPointInstancer pointInstancer(*primIt);
			if (!pointInstancer) {
				continue;
			}

			hasPointInstancer = true;
			SdfPathVector targets;
			if (pointInstancer.GetPrototypesRel().GetTargets(&targets)) {
				for (const SdfPath& target : targets) {
					if (prototypeRootStrings.insert(target.GetString()).second) {
						prototypeRoots.push_back(target);
					}
				}
			}
		}
		if (!hasPointInstancer) {
			return std::nullopt;
		}

		UsdGeomMesh firstRootMesh;
		auto meshRange = UsdPrimRange(stage->GetPseudoRoot());
		for (auto primIt = meshRange.begin(); primIt != meshRange.end(); ++primIt) {
			UsdGeomMesh mesh(*primIt);
			if (mesh && !pathHasAnyPrefix(mesh.GetPrim().GetPath(), prototypeRoots)) {
				firstRootMesh = mesh;
				break;
			}
		}

		CLodCacheLoader::MeshCacheIdentity assemblyIdentity{};
		if (firstRootMesh) {
			assemblyIdentity = CLodCacheLoader::BuildIdentity(firstRootMesh, stage, "CLodAssembly", geomTimeCode, sourceIdentifier);
		}
		else {
			assemblyIdentity.sourceIdentifier = sourceIdentifier;
			if (assemblyIdentity.sourceIdentifier.empty() && stage->GetRootLayer()) {
				assemblyIdentity.sourceIdentifier = stage->GetRootLayer()->GetIdentifier();
			}
			assemblyIdentity.subsetName = "CLodAssembly";
		}

		const UsdPrim defaultPrim = stage->GetDefaultPrim();
		assemblyIdentity.primPath = defaultPrim ? defaultPrim.GetPath().GetString() + "/__CLodAssembly" : "/__CLodAssembly";
		assemblyIdentity.sourceIdentifier += "#usd_point_instancer_clod_assembly=8#assembly_double_sided_coverage=1#assembly_scaled_coverage_rays=1#weighted_voxel_coverage=1#hierarchical_voxel_sggx=2";
		return assemblyIdentity;
	}

	void TryLoadPointInstancerAssemblyMesh(
		const UsdStageRefPtr& stage,
		UsdTimeCode geomTimeCode,
		const std::string& sourceIdentifier)
	{
		auto tryIdentity = [&](const std::string& identitySource) -> bool {
			auto identity = BuildPointInstancerAssemblyIdentity(stage, identitySource, geomTimeCode);
			if (!identity) {
				return false;
			}

			auto prebuilt = CLodCacheLoader::TryLoadPrebuilt(*identity);
			if (!prebuilt || prebuilt->assemblyInstances.empty()) {
				return false;
			}

			MeshIngestBuilder ingest(0u, 0u, 0u, GetDefaultBuilderSettings());
			auto mesh = ingest.Build(Material::GetDefaultMaterial(), std::move(prebuilt), MeshCpuDataPolicy::ReleaseAfterUpload);
			if (!mesh) {
				return false;
			}

			loadingCache.stageAssemblyMeshes.push_back(std::move(mesh));
			spdlog::info(
				"USD point-instancer CLod assembly cache loaded for synthetic stage renderable using source id '{}'.",
				identitySource);
			return true;
		};

		if (tryIdentity(sourceIdentifier)) {
			return;
		}
		if (!sourceIdentifier.empty()) {
			(void)tryIdentity({});
		}
	}

	std::vector<std::shared_ptr<Mesh>> ProcessMesh(
		const UsdGeomMesh& mesh,
		const pxr::UsdStageRefPtr& stage,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		bool isUSDZ,
		const UsdSkelCache& skelCache,
		VtTokenArray& skelJointOrderRaw,
		VtTokenArray& skelJointOrderMapped)
	{
		ZoneScopedN("USDLoader::ProcessMesh");
		const auto meshPathText = mesh.GetPrim().GetPath().GetString();
		ZoneText(meshPathText.data(), meshPathText.size());
		(void)stage;
		(void)metersPerUnit;
		(void)upRot;
		(void)directory;
		(void)isUSDZ;
		(void)skelCache;
		(void)skelJointOrderRaw;
		(void)skelJointOrderMapped;

		const std::string cacheKey = mesh.GetPrim().GetPath().GetString();
		if (loadingCache.meshCache.contains(cacheKey)) {
			ZoneScopedN("USDLoader::ProcessMesh::MeshCacheHit");
			return loadingCache.meshCache[cacheKey];
		}

		std::vector<std::shared_ptr<Mesh>> outMeshes;
		auto preprocessedIt = loadingCache.preprocessedMeshCache.find(cacheKey);
		if (preprocessedIt == loadingCache.preprocessedMeshCache.end()) {
			if (auto skippedIt = loadingCache.skippedPreprocessedMeshReasons.find(cacheKey);
				skippedIt != loadingCache.skippedPreprocessedMeshReasons.end()) {
				spdlog::debug(
					"USD mesh '{}' was intentionally skipped during preprocessing: {}.",
					cacheKey,
					skippedIt->second);
			}
			else {
				spdlog::warn("USD mesh '{}' was not present in the preprocessed mesh cache.", cacheKey);
			}
			loadingCache.meshCache[cacheKey] = outMeshes;
			return outMeshes;
		}

		PreprocessedMeshRecord& record = preprocessedIt->second;
		outMeshes.reserve(record.subsets.size());
		TracyPlot("SARP.Import.USD.ProcessMesh.Subsets", static_cast<int64_t>(record.subsets.size()));
		for (PreprocessedMeshSubset& subset : record.subsets) {
			ZoneScopedN("USDLoader::ProcessMesh::Subset");
			auto& result = subset.result;
			std::shared_ptr<Material> material;
			{
				ZoneScopedN("USDLoader::ProcessMesh::Subset::ResolveMaterialForMesh");
				const bool subsetCameFromOwnerMesh =
					subset.staticTextureOverrideSourceName.empty() ||
					subset.staticTextureOverrideSourceName == mesh.GetPrim().GetName().GetString();
				material = ResolveMaterialForMesh(
					subset.material,
					result.ingest.GetUvSets(),
					record.authoredDoubleSided || subset.inferredDoubleSided || result.forceDoubleSidedPreview,
					subsetCameFromOwnerMesh ? mesh.GetPrim() : UsdPrim(),
					std::addressof(result),
					subset.staticTextureOverrideSourceName);
			}
			std::shared_ptr<Mesh> mPtr;
			{
				ZoneScopedN("USDLoader::ProcessMesh::Subset::BuildMeshFromIngest");
				auto atlasBakeData = BuildObjectReyesAtlasBakeDataForMesh(result);
				mPtr = result.ingest.Build(material, std::move(result.prebuiltData), MeshCpuDataPolicy::ReleaseAfterUpload);
				if (mPtr && atlasBakeData) {
					mPtr->SetObjectReyesAtlasBakeData(std::move(atlasBakeData));
				}
			}
			if (mPtr != nullptr) {
				{
					ZoneScopedN("USDLoader::ProcessMesh::Subset::ApplyBrNiflySkinMetadata");
					auto jointNames = GetBrNiflyJointNames(mesh.GetPrim());
					if (!jointNames.empty()) {
						mPtr->SetSkinJointNames(std::move(jointNames));
					}
					auto jointSourceIndices = GetBrNiflyJointSourceIndices(mesh.GetPrim());
					if (!jointSourceIndices.empty()) {
						mPtr->SetSkinJointSourceIndices(std::move(jointSourceIndices));
					}
					auto skinToBoneTransforms = GetBrNiflySkinToBoneTransforms(mesh.GetPrim());
					if (!skinToBoneTransforms.empty()) {
						mPtr->SetSkinInverseBindMatrices(std::move(skinToBoneTransforms));
					}
				}
				outMeshes.push_back(mPtr);
			}
		}

		loadingCache.meshCache[cacheKey] = outMeshes;
		return outMeshes;
	}

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

	static GfVec3d ExtractUsdMatrixTranslation(const GfMatrix4d& matrix)
	{
		return matrix.Transform(GfVec3d(0.0));
	}

	static double UsdDistance(const GfVec3d& a, const GfVec3d& b)
	{
		const GfVec3d delta = a - b;
		return delta.GetLength();
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

	static std::string_view UsdJointLeafName(std::string_view jointName)
	{
		const size_t slash = jointName.find_last_of('/');
		return slash == std::string_view::npos ? jointName : jointName.substr(slash + 1u);
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
		bool sanitizeAssemblyHierarchy = false,
		const UsdStageRefPtr& stage = nullptr)
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

	static std::optional<CLodCacheLoader::MeshCacheIdentity> BuildAssetAssemblyIdentity(
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

	static std::vector<AssetAssemblyBucketInfo> DiscoverAssetAssemblyBuckets(
		const UsdStageRefPtr& stage,
		UsdSkelCache& skelCache,
		double metersPerUnit,
		UsdTimeCode geomTimeCode,
		const InMemoryStageOptions& stageOptions,
		const ImportSettings& importSettings,
		std::string* fallbackReason = nullptr)
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

	static std::shared_ptr<Mesh> BuildMeshFromAssetAssemblyPrebuilt(
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

	static bool TryLoadAssetAssemblyMeshes(
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

	static bool BuildAssetAssemblyMeshesFromPreprocessedData(
		const UsdStageRefPtr& stage,
		const StageImportContext& stageContext,
		const ImportSettings& importSettings,
		const InMemoryStageOptions& options,
		const std::string& sourceIdentifier,
		UsdTimeCode geomTimeCode,
		const std::vector<AssetAssemblyBucketInfo>& buckets,
		std::vector<std::shared_ptr<Mesh>>& meshes,
		std::string* outFailureReason = nullptr)
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

	static std::shared_ptr<Scene> CreateCollapsedAssetAssemblyScene(
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

	std::shared_ptr<Scene> LoadModel(std::string filePath, const ImportSettings& importSettings) {

		UsdStageRefPtr stage = UsdStage::Open(filePath);
		if (!stage) {
			spdlog::error("USD stage open failed for {}", filePath);
			return nullptr;
		}

		InMemoryStageOptions options{};
		options.sourceIdentifier = filePath;
		options.sourceDirectory = std::filesystem::path(filePath).parent_path().string();
		options.layerIdentifierHint = std::filesystem::path(filePath).filename().string();
		options.isUsdPackage = std::filesystem::path(filePath).extension() == ".usdz";

		return LoadModelFromStage(stage, options, importSettings);
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

	std::shared_ptr<Scene> LoadModelFromFile(
		const std::string& filePath,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings) {
		UsdStageRefPtr stage = UsdStage::Open(filePath);
		if (!stage) {
			spdlog::error("USD stage open failed for {}", filePath);
			return nullptr;
		}

		return LoadModelFromStage(stage, options, importSettings);
	}

	std::optional<ImportedAssetPayload> LoadImportedAssetFromUsdBytes(
		const std::string& usdText,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings,
		ImportTimingStats* timingStats) {
		ZoneScopedN("USDLoader::LoadImportedAssetFromUsdBytes");
		ZoneText(options.sourceIdentifier.data(), options.sourceIdentifier.size());
		TracyPlot("SARP.Import.USD.InMemoryBytes", static_cast<int64_t>(usdText.size()));
		const std::string identifierHint = options.layerIdentifierHint.empty() ? std::string("in_memory.usda") : options.layerIdentifierHint;
		SdfLayerRefPtr rootLayer = SdfLayer::CreateAnonymous(identifierHint);
		{
			ZoneScopedN("USDLoader::LoadImportedAssetFromUsdBytes::ImportLayerFromString");
			const auto begin = std::chrono::steady_clock::now();
			if (!rootLayer || !rootLayer->ImportFromString(usdText)) {
				if (timingStats) {
					timingStats->layerImportMs += ElapsedMs(begin);
				}
				spdlog::error("Failed to import in-memory USD payload layer '{}'.", identifierHint);
				return std::nullopt;
			}
			if (timingStats) {
				timingStats->layerImportMs += ElapsedMs(begin);
			}
		}

		UsdStageRefPtr stage;
		{
			ZoneScopedN("USDLoader::LoadImportedAssetFromUsdBytes::UsdStageOpen");
			const auto begin = std::chrono::steady_clock::now();
			stage = UsdStage::Open(rootLayer);
			if (timingStats) {
				timingStats->stageOpenMs += ElapsedMs(begin);
			}
		}
		if (!stage) {
			spdlog::error("Failed to open in-memory USD payload stage '{}'.", identifierHint);
			return std::nullopt;
		}

		return LoadImportedAssetFromStage(stage, options, importSettings, timingStats);
	}

	std::shared_ptr<Scene> LoadModelFromUsdBytes(
		const std::string& usdText,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings) {
		const std::string identifierHint = options.layerIdentifierHint.empty() ? std::string("in_memory.usda") : options.layerIdentifierHint;
		SdfLayerRefPtr rootLayer = SdfLayer::CreateAnonymous(identifierHint);
		if (!rootLayer || !rootLayer->ImportFromString(usdText)) {
			spdlog::error("Failed to import in-memory USD layer '{}'.", identifierHint);
			return nullptr;
		}

		UsdStageRefPtr stage = UsdStage::Open(rootLayer);
		if (!stage) {
			spdlog::error("Failed to open in-memory USD stage '{}'.", identifierHint);
			return nullptr;
		}

		return LoadModelFromStage(stage, options, importSettings);
	}

	std::shared_ptr<Scene> LoadModel(std::string filePath) {
		return LoadModel(std::move(filePath), ImportSettings{});
	}

}
