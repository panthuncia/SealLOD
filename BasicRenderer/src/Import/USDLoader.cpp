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

#include <nlohmann/json.hpp>
#include <tracy/Tracy.hpp>

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
#include "Mesh/ClusterLODUtilities.h"
#include "Animation/Skeleton.h"
#include "Scene/Components.h"
#include "Animation/AnimationController.h"
#include "Managers/Singletons/SettingsManager.h"
#include "Managers/Singletons/TaskSchedulerManager.h"
#include "Managers/Singletons/TextureProcessingManager.h"

#include "Import/USDLoader.h"
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
		//std::unordered_map<std::string, std::shared_ptr<UsdSkelSkeleton>> unprocessedSkeletons;
		std::unordered_map<std::string, UsdPrim> primsWithSkeletons;
		std::unordered_map<std::string, std::shared_ptr<Skeleton>> skeletonMap;
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
			primsWithSkeletons.clear();
			skeletonMap.clear();
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
			const std::array<std::filesystem::path, 2> candidates = {
				root / normalizedRelative,
				root / "textures" / withoutTexturesPrefix
			};
			for (const auto& candidate : candidates) {
				ec.clear();
				if (std::filesystem::is_regular_file(candidate, ec)) {
					auto canonical = std::filesystem::weakly_canonical(candidate, ec);
					return ec ? candidate : canonical;
				}
			}
		}

		return std::nullopt;
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
		auto ctx = stage->GetPathResolverContext();
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
			spdlog::warn("USDLoader: unable to resolve texture '{}'", logicalPath);
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
		else if (std::shared_ptr<ArAsset> arAsset = resolver.OpenAsset(resolved)) {
			tex = LoadTextureFromMemory(
				static_cast<const void*>(arAsset->GetBuffer().get()),
				arAsset->GetSize(),
				nullptr,
				{},
				preferSRGB);
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
				spdlog::warn("Unknown texture input: {}", name.GetString());
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
				if (tex && NormalTextureNeedsReconstructedZ(tex->Description().format)) {
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
			if (NormalTextureNeedsReconstructedZ(result.normal.texture->Description().format)) {
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
					spdlog::warn(
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

		ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings();
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
		const unsigned int vertexSize = first.ingest.GetVertexSize();
		const unsigned int skinningVertexSize = first.ingest.GetSkinningVertexSize();
		const unsigned int vertexFlags = first.ingest.GetFlags();
		if (skinningVertexSize != 0u) {
			spdlog::warn("Object Reyes atlas bake skipped: skinned meshes are not supported.");
			return std::nullopt;
		}

		std::vector<br::import::ObjectReyesAtlasSourceMesh> sources;
		sources.reserve(group.size());
		float texelsPerUnitSum = 0.0f;
		std::uint32_t texelsPerUnitCount = 0u;
		for (std::size_t groupEntry = 0; groupEntry < group.size(); ++groupEntry) {
			const std::size_t index = group[groupEntry];
			const MeshPreprocessWorkItem& item = workItems[index];
			if (!preprocessed[index] ||
				!item.subsets.empty() ||
				item.skinQ ||
				item.extractOptions.objectSurfaceSamplingMode != ObjectSurfaceSamplingMode::AtlasBakedHeight) {
				return std::nullopt;
			}
			const MeshPreprocessResult& result = preprocessed[index].value();
			if (result.ingest.GetVertexSize() != vertexSize ||
				result.ingest.GetSkinningVertexSize() != skinningVertexSize ||
				result.ingest.GetFlags() != vertexFlags) {
				spdlog::warn(
					"Object Reyes atlas bake skipped under '{}': incompatible sibling vertex layout.",
					ParentPrimPath(workItems[group.front()].mesh.GetPrim().GetPath().GetString()));
				return std::nullopt;
			}
			if (std::isfinite(result.objectSurfaceTexelDensity) && result.objectSurfaceTexelDensity > 0.0f) {
				texelsPerUnitSum += result.objectSurfaceTexelDensity;
				++texelsPerUnitCount;
			}
			sources.push_back(br::import::ObjectReyesAtlasSourceMesh{
				.vertices = std::addressof(result.ingest.GetVertices()),
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
		ClusterLODBuilderSettings builderSettings = GetDefaultBuilderSettings();
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
			identity.sourceIdentifier += "#object_reyes_atlas_baked_height_version=13";
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
		const std::string& sourceIdentifierOverride = {})
	{
		ZoneScopedN("USDLoader::PreprocessAllMeshes");
		ZoneText(sourceIdentifierOverride.data(), sourceIdentifierOverride.size());
		loadingCache.preprocessedMeshCache.clear();
		loadingCache.skippedPreprocessedMeshReasons.clear();

		const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);
		UsdSkelCache preprocessSkelCache;
		std::vector<MeshPreprocessWorkItem> workItems;
		auto markSkippedMesh = [](const std::string& meshPath, std::string reason) {
			if (!meshPath.empty()) {
				loadingCache.skippedPreprocessedMeshReasons.emplace(meshPath, std::move(reason));
			}
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

				if (IsUnsupportedBrNiflySkinnedMesh(mesh)) {
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
						preprocessSkelCache.Populate(UsdSkelRoot(skel.GetPrim()), UsdPrimDefaultPredicate);
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
					workItems.push_back(MeshPreprocessWorkItem{
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
						workItems.insert(
							workItems.end(),
							std::make_move_iterator(combinedSubsetWorkItems.begin()),
							std::make_move_iterator(combinedSubsetWorkItems.end()));
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
				if (group.empty()) {
					continue;
				}
				auto bakedSubsets = TryBuildObjectReyesAtlasBakedParentGroup(group, workItems, preprocessed, stageOptions);
				if (!bakedSubsets) {
					spdlog::warn(
						"Object Reyes atlas-baked height failed under '{}'; disabling Object Reyes geometric displacement for that parent.",
						parentPath);
					DisableObjectReyesForGroup(group, preprocessed);
					continue;
				}

				const std::size_t ownerIndex = group.front();
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

				for (std::size_t groupEntry = 0u; groupEntry < group.size(); ++groupEntry) {
					const std::size_t index = group[groupEntry];
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
		assemblyIdentity.sourceIdentifier += "#usd_point_instancer_clod_assembly=3";
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

	std::shared_ptr<Skeleton> BuildPayloadSkeleton(
		const UsdSkelSkeleton& skel,
		const VtTokenArray& rawJointOrder,
		const UsdSkelSkeletonQuery& skelQuery,
		double metersPerUnit)
	{
		ZoneScopedN("USDLoader::BuildPayloadSkeleton");
		const auto skeletonPath = skel.GetPrim().GetPath().GetString();
		ZoneText(skeletonPath.data(), skeletonPath.size());
		if (loadingCache.skeletonMap.contains(skel.GetPrim().GetPath().GetString())) {
			return loadingCache.skeletonMap[skel.GetPrim().GetPath().GetString()];
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
		boneNames.reserve(rawJointOrder.size());
		parentIndices.reserve(rawJointOrder.size());
		inverseBindMatrices.reserve(rawJointOrder.size());

		for (size_t i = 0; i < rawJointOrder.size(); ++i) {
			boneNames.push_back(rawJointOrder[i].GetString());
			parentIndices.push_back(topology.GetParent(i));

			const GfMatrix4d bindMatrix = i < bindXforms.size() ? bindXforms[i] : GfMatrix4d(1.0);
			inverseBindMatrices.push_back(DirectX::XMMatrixInverse(nullptr, DirectXMatrixFromUsdMatrix(bindMatrix, metersPerUnit)));
		}

		auto skeleton = std::make_shared<Skeleton>(std::move(boneNames), std::move(parentIndices), std::move(inverseBindMatrices));
		loadingCache.skeletonMap[skel.GetPrim().GetPath().GetString()] = skeleton;
		return skeleton;
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
				skelCache.Populate(UsdSkelRoot(skel.GetPrim()), UsdPrimDefaultPredicate);
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
				skelCache.Populate(UsdSkelRoot(skel.GetPrim()), UsdPrimDefaultPredicate);
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
		if (!stageContext.directory.empty() &&
			std::find(loadingCache.textureSearchRoots.begin(), loadingCache.textureSearchRoots.end(), stageContext.directory) == loadingCache.textureSearchRoots.end()) {
			loadingCache.textureSearchRoots.push_back(stageContext.directory);
		}

		auto scene = std::make_shared<Scene>();

		UsdSkelCache skelCache;

		PreprocessAllMeshes(stage, stageContext.metersPerUnit, stageContext.directory, stageContext.isUSDZ, importSettings, options, options.sourceIdentifier);

		ParseNodeHierarchy(scene, stage, stageContext.metersPerUnit, stageContext.upRot, stageContext.directory, skelCache, stageContext.isUSDZ);

		loadingCache.Clear();

		return scene;
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
		if (!stageContext.directory.empty() &&
			std::find(loadingCache.textureSearchRoots.begin(), loadingCache.textureSearchRoots.end(), stageContext.directory) == loadingCache.textureSearchRoots.end()) {
			loadingCache.textureSearchRoots.push_back(stageContext.directory);
		}

		try {
			UsdSkelCache skelCache;

			{
				const auto begin = std::chrono::steady_clock::now();
				PreprocessAllMeshes(stage, stageContext.metersPerUnit, stageContext.directory, stageContext.isUSDZ, importSettings, options, options.sourceIdentifier);
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

		return LoadImportedAssetFromStage(stage, options, importSettings, timingStats);
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
