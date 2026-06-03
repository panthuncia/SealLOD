#include <spdlog/spdlog.h>
#include <DirectXMath.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <unordered_set>
#include <cmath>

#include <nlohmann/json.hpp>

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
#include "Import/USDGeometryExtractor.h"

namespace USDLoader {

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
		MeshPreprocessResult result;
		bool inferredDoubleSided = false;

		PreprocessedMeshSubset(UsdShadeMaterial m, MeshPreprocessResult&& r, bool inferred)
			: material(std::move(m)), result(std::move(r)), inferredDoubleSided(inferred) {}
	};

	struct PreprocessedMeshRecord {
		bool authoredDoubleSided = false;
		std::vector<PreprocessedMeshSubset> subsets;
	};

	struct LoadingCaches {
		std::unordered_map<std::string, MaterialTemplateRecord> materialTemplateCache;
        std::unordered_map<std::string, std::shared_ptr<Material>> resolvedMaterialCache;
		std::unordered_map<std::string, std::vector<std::shared_ptr<Mesh>>> meshCache;
		std::unordered_map<std::string, PreprocessedMeshRecord> preprocessedMeshCache;
		std::unordered_map<std::string, std::shared_ptr<TextureAsset>> textureCache;
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
			textureCache.clear();
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

	void ProcessTexture(MaterialDescription& result, const UsdShadeConnectionSourceInfo& src, const UsdStageRefPtr& stage, const TfToken& name, const UsdShadeMaterial& material) {
		if (auto srcShader = UsdShadeShader(src.source)) {
			TfToken srcId;
			srcShader.GetIdAttr().Get(&srcId);

			if (srcId == TfToken("UsdUVTexture")) {
				// load the texture and stash it
				SdfAssetPath asset;
				srcShader.GetInput(TfToken("file")).Get(&asset);
				// Resolve asset path
				std::string logicalPath = asset.GetResolvedPath();

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
				const std::string cacheKey = BuildUsdTextureCacheKey(logicalPath, semantic, preferSRGB, normalConvention);

				if (!loadingCache.textureCache.contains(cacheKey)) {
					spdlog::debug("Found texture  {} in material", src.source.GetPrim().GetName().GetString());
					//auto texPath = asset.GetAssetPath();
					//spdlog::info("Loading texture from path: {}", texPath);

					auto& resolver = ArGetResolver();
					auto ctx = stage->GetPathResolverContext();
					ArResolverContextBinder binder(ctx);

					ArResolvedPath resolved = resolver.Resolve(logicalPath);

					TextureFileMeta cacheProbeMeta{};
					cacheProbeMeta.filePath = resolved.GetPathString();
					cacheProbeMeta.preferSRGB = preferSRGB;
					cacheProbeMeta.processing = MakeMaterialTextureProcessingSettings(semantic, preferSRGB, cacheKey, false, normalConvention);
					const std::wstring cachePath = TextureProcessingManager::GetInstance().GetExistingCachePathForFile(cacheProbeMeta);
					if (!cachePath.empty()) {
						auto tex = LoadTextureFromFile(cachePath, nullptr, preferSRGB);
						tex->Meta().filePath = cacheProbeMeta.filePath;
						tex->Meta().isProcessingCacheArtifact = true;
						tex->Meta().preferSRGB = preferSRGB;
						tex->SetProcessingSettings(cacheProbeMeta.processing);
						loadingCache.textureCache[cacheKey] = tex;
						spdlog::debug("USDLoader: texture processing cache hit for '{}' -> '{}'", resolved.GetPathString(), ws2s(cachePath));
					}
					else {
						// Open the asset
						std::shared_ptr<ArAsset> arAsset = resolver.OpenAsset(resolved);
						if (!arAsset) {
							throw std::runtime_error(
								"Unable to open asset at " + logicalPath);
						}

						auto tex = LoadTextureFromMemory(
							static_cast<const void*>(arAsset->GetBuffer().get()),
							arAsset->GetSize(),
							nullptr,
							{},              // default flags; loader will force WIC sRGB/linear as needed
							preferSRGB);
						tex->SetProcessingSettings(MakeMaterialTextureProcessingSettings(semantic, preferSRGB, cacheKey, false, normalConvention));

						tex->SetGenerateMipmaps(true); // TODO: There will be textures where we don't want this

						loadingCache.textureCache[cacheKey] = tex;
					}

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
			std::string logicalPath = asset.GetResolvedPath();
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
			if (texIt != loadingCache.textureCache.end()) {
				auto tex = texIt->second;
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
					result.negateNormals =
						tex->Meta().fileType == ImageFiletype::DDS ||
						(tex->Meta().isProcessingCacheArtifact && tex->Meta().processing.semantic == TextureSemantic::Normal);
					result.invertNormalGreen = false;
				}
				if (name == TfToken("emissiveColor") && IsBlack(result.emissiveColor)) {
					result.emissiveColor = { 1.0f, 1.0f, 1.0f, 1.0f };
				}
			}
		}
	}

	void ProcessDisplacementTerminal(
		MaterialDescription& result,
		const pxr::UsdShadeMaterial& material,
		const UsdStageRefPtr& stage,
		std::unordered_map<ResolveCacheKey, ResolvedProducer, ResolveCacheKeyHash>& cache)
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
				ProcessTexture(result, src, stage, TfToken("displacement"), material);
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
						ProcessTexture(result, inputSource, stage, pxr::TfToken("displacement"), material);
					}
				}
			}
		}
	}

	MaterialDescription ParseMaterialGraph(
		const pxr::UsdShadeMaterial& material,
		const std::string& directory,
		const UsdStageRefPtr& stage,
		bool isUSDZ)
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
					ProcessTexture(result, src, stage, *legacyTextureName, material);
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

        ProcessDisplacementTerminal(result, material, stage, cache);

		//Post-process to assign 1.0 to undefined factors with a valid texture
		ForEachMaterialTextureBinding(result, [](TextureAndConstant& binding) {
			if (binding.texture && !binding.factor.HasValue()) {
				binding.factor = 1.0f; // Unlike glTF, USD does not require a factor to be set if a texture is present
			}
		});

		ApplyBrniflyMaterialMetadata(result, material.GetPrim());

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

	void ProcessMaterial(const pxr::UsdShadeMaterial& material, const pxr::UsdStageRefPtr& stage, bool isUSDZ, const std::string& directory) {
		if (!material) {
			return;
		}

		if (loadingCache.materialTemplateCache.contains(material.GetPrim().GetPath().GetString())) {
			spdlog::debug("Material {} already processed, skipping.", material.GetPrim().GetPath().GetString());
			return; // Already processed
		}

		spdlog::debug("Processing material: {}", material.GetPrim().GetPath().GetString());

		auto materialDesc = ParseMaterialGraph(material, directory, stage, isUSDZ);
        MaterialTemplateRecord record;
        record.desc = std::move(materialDesc);
        record.referencedUvSetNames = CollectReferencedUvSetNames(record.desc);
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
			std::to_string(resolvedDesc.brniflyModelSpaceNormals ? 1 : 0);
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

		return name.starts_with("l1_") ||
			name.starts_with("l2_") ||
			name.starts_with("l3_") ||
			name.contains("_l1_") ||
			name.contains("_l2_") ||
			name.contains("_l3_") ||
			name.starts_with("lod_") ||
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
		if (IsBrNiflyLODMeshName(prim.GetName().GetString())) {
			return true;
		}

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
		const UsdPrim& meshPrim = UsdPrim()) {
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
        resolvedDesc.forceDoubleSided = resolvedDesc.forceDoubleSided || forceDoubleSided;

        const std::string cacheKey = BuildResolvedMaterialCacheKey(materialPath, resolvedDesc);
        auto resolvedIt = loadingCache.resolvedMaterialCache.find(cacheKey);
        if (resolvedIt != loadingCache.resolvedMaterialCache.end()) {
            return resolvedIt->second;
        }

        auto runtimeMaterial = Material::CreateShared(resolvedDesc);
        loadingCache.resolvedMaterialCache[cacheKey] = runtimeMaterial;
        return runtimeMaterial;
    }

	USDGeometryExtractor::ExtractOptions BuildGeometryExtractOptions(
		const UsdGeomMesh& mesh,
		const UsdShadeMaterial& material)
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
		const bool temporaryBlockedOverlay = desc.brniflyDecal || desc.brniflyDynamicDecal;
		if (desc.brniflyVertexAlpha && desc.blendState == BlendState::BLEND_STATE_MASK && temporaryBlockedOverlay) {
			options.vertexAlphaCutoff = std::clamp(desc.alphaCutoff, 0.0f, 1.0f);
		}
		return options;
	}

	bool ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(const USDGeometryExtractor::ExtractOptions& options)
	{
		return options.vertexAlphaCutoff.has_value() &&
			options.brniflyVertexAlpha &&
			(options.brniflyDecal || options.brniflyDynamicDecal);
	}

	void PreprocessAllMeshes(
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		const std::string& directory,
		bool isUSDZ,
		const ImportSettings& importSettings,
		const std::string& sourceIdentifierOverride = {})
	{
		struct MeshPreprocessWorkItem {
			std::string meshPath;
			UsdGeomMesh mesh;
			std::optional<UsdGeomSubset> subset;
			UsdShadeMaterial material;
			std::vector<std::string> requiredUvSetNames;
			std::optional<UsdSkelSkinningQuery> skinQ;
			VtTokenArray skelJointOrderRaw;
			VtTokenArray skelJointOrderMapped;
			USDGeometryExtractor::ExtractOptions extractOptions;
			bool authoredDoubleSided = false;
			bool inferredDoubleSided = false;
		};

		loadingCache.preprocessedMeshCache.clear();

		const UsdTimeCode geomTimeCode = GetUsdGeometrySampleTime(stage);
		UsdSkelCache preprocessSkelCache;
		std::vector<MeshPreprocessWorkItem> workItems;

		std::function<void(const UsdPrim&)> gatherMeshJobs = [&](const UsdPrim& prim) {
			if (prim.IsA<UsdGeomImageable>()) {
				UsdGeomImageable imageable(prim);
				if (imageable.ComputeVisibility(geomTimeCode) == UsdGeomTokens->invisible) {
					return;
				}
			}

			UsdGeomMesh mesh(prim);
			if (mesh) {
				if (IsBrNiflyCollisionMesh(mesh)) {
					spdlog::info("Skipping BRNifly collision mesh '{}'.", mesh.GetPrim().GetPath().GetString());
					return;
				}

				if (IsUnsupportedBrNiflySkinnedMesh(mesh)) {
					spdlog::info(
						"Skipping BRNifly skinned mesh '{}' until NIF skeleton pose updates are supported.",
						mesh.GetPrim().GetPath().GetString());
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

				const std::string meshPath = mesh.GetPrim().GetPath().GetString();
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
					ProcessMaterial(mat, stage, isUSDZ, directory);
					const auto extractOptions = BuildGeometryExtractOptions(mesh, mat);
					if (ShouldTemporarilyBlockBrniflyVertexAlphaOverlay(extractOptions)) {
						spdlog::info(
							"Temporarily skipping BRNifly vertex-alpha overlay mesh '{}' material '{}' (zwrite={}, decal={}, dynamicDecal={}, cutoff={}).",
							meshPath,
							mat ? mat.GetPrim().GetPath().GetString() : std::string("<unbound>"),
							extractOptions.brniflyZBufferWrite,
							extractOptions.brniflyDecal,
							extractOptions.brniflyDynamicDecal,
							extractOptions.vertexAlphaCutoff.value());
						return;
					}
					const bool inferredDoubleSided = ShouldForceDoubleSidedByName(mat, std::nullopt, importSettings);
					workItems.push_back(MeshPreprocessWorkItem{
						.meshPath = meshPath,
						.mesh = mesh,
						.subset = std::nullopt,
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
					for (const auto& subset : subsets) {
						auto mat = UsdShadeMaterialBindingAPI(subset).ComputeBoundMaterial();
						ProcessMaterial(mat, stage, isUSDZ, directory);
						const auto extractOptions = BuildGeometryExtractOptions(mesh, mat);
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
						workItems.push_back(MeshPreprocessWorkItem{
							.meshPath = meshPath,
							.mesh = mesh,
							.subset = subset,
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
				}
			}

			for (auto child : prim.GetFilteredChildren(UsdTraverseInstanceProxies())) {
				gatherMeshJobs(child);
			}
		};
		gatherMeshJobs(stage->GetPseudoRoot());

		spdlog::debug("USD mesh preprocessing: gathered {} mesh/subset job(s).", workItems.size());
		std::vector<std::optional<MeshPreprocessResult>> preprocessed(workItems.size());
		TaskSchedulerManager::GetInstance().ParallelFor("USDLoader::PreprocessMeshes", workItems.size(), [&](size_t workIndex) {
			const MeshPreprocessWorkItem& workItem = workItems[workIndex];
			preprocessed[workIndex] = USDGeometryExtractor::ExtractSubMesh(
				workItem.mesh,
				workItem.subset,
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

		for (size_t workIndex = 0; workIndex < workItems.size(); ++workIndex) {
			if (!preprocessed[workIndex].has_value()) {
				throw std::runtime_error("Missing preprocessed USD mesh data");
			}

			const MeshPreprocessWorkItem& workItem = workItems[workIndex];
			auto& record = loadingCache.preprocessedMeshCache[workItem.meshPath];
			record.authoredDoubleSided = workItem.authoredDoubleSided;
			record.subsets.emplace_back(workItem.material, std::move(preprocessed[workIndex].value()), workItem.inferredDoubleSided);
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
		(void)stage;
		(void)metersPerUnit;
		(void)upRot;
		(void)directory;
		(void)isUSDZ;
		(void)skelCache;
		(void)skelJointOrderRaw;
		(void)skelJointOrderMapped;

		auto& cacheKey = mesh.GetPrim().GetPath().GetString();
		if (loadingCache.meshCache.contains(cacheKey)) {
			return loadingCache.meshCache[cacheKey];
		}

		std::vector<std::shared_ptr<Mesh>> outMeshes;
		auto preprocessedIt = loadingCache.preprocessedMeshCache.find(cacheKey);
		if (preprocessedIt == loadingCache.preprocessedMeshCache.end()) {
			spdlog::warn("USD mesh '{}' was not present in the preprocessed mesh cache.", cacheKey);
			loadingCache.meshCache[cacheKey] = outMeshes;
			return outMeshes;
		}

		PreprocessedMeshRecord& record = preprocessedIt->second;
		outMeshes.reserve(record.subsets.size());
		for (PreprocessedMeshSubset& subset : record.subsets) {
			auto& result = subset.result;
			auto material = ResolveMaterialForMesh(
				subset.material,
				result.ingest.GetUvSets(),
				record.authoredDoubleSided || subset.inferredDoubleSided || result.forceDoubleSidedPreview,
				mesh.GetPrim());
			auto mPtr = result.ingest.Build(material, std::move(result.prebuiltData), MeshCpuDataPolicy::ReleaseAfterUpload);
			if (mPtr != nullptr) {
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

	std::vector<std::shared_ptr<Mesh>> ProcessMeshForPayload(
		const UsdPrim& prim,
		UsdSkelCache& skelCache,
		const UsdStageRefPtr& stage,
		double metersPerUnit,
		GfRotation upRot,
		const std::string& directory,
		bool isUSDZ)
	{
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

		auto skinningQuery = USDGeometryExtractor::GetSkinningQuery(mesh, skelCache);
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
		if (skeleton) {
			for (auto& processedMesh : processedMeshes) {
				if (processedMesh) {
					processedMesh->SetBaseSkin(skeleton);
				}
			}
		}
		return processedMeshes;
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

				auto meshes = ProcessMeshForPayload(prim, skelCache, stage, metersPerUnit, upRot, directory, isUSDZ);
				if (!meshes.empty()) {
					RenderablePartPayload part;
					part.localMatrix = worldMatrix;
					part.name = prim.GetName().GetString();

					bool hasSkinnedMesh = false;
					for (const auto& mesh : meshes) {
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

		recurse(stage->GetPseudoRoot(), DirectX::XMMatrixIdentity(), false);
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

		auto scene = std::make_shared<Scene>();

		UsdSkelCache skelCache;

		PreprocessAllMeshes(stage, stageContext.metersPerUnit, stageContext.directory, stageContext.isUSDZ, importSettings, options.sourceIdentifier);

		ParseNodeHierarchy(scene, stage, stageContext.metersPerUnit, stageContext.upRot, stageContext.directory, skelCache, stageContext.isUSDZ);

		loadingCache.Clear();

		return scene;
	}

	std::optional<ImportedAssetPayload> LoadImportedAssetFromStage(
		const UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& importSettings) {
		if (!stage) {
			spdlog::error("USD payload stage open failed for in-memory source '{}'", options.sourceIdentifier);
			return std::nullopt;
		}

		auto ctx = stage->GetPathResolverContext();
		ArResolverContextBinder binder(ctx);

		const auto stageContext = MakeStageImportContext(stage, options);

		try {
			UsdSkelCache skelCache;

			PreprocessAllMeshes(stage, stageContext.metersPerUnit, stageContext.directory, stageContext.isUSDZ, importSettings, options.sourceIdentifier);
			auto payload = ParseImportedAssetPayload(stage, stageContext.metersPerUnit, stageContext.upRot, stageContext.directory, skelCache, stageContext.isUSDZ);
			loadingCache.Clear();
			return payload;
		}
		catch (...) {
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
		const ImportSettings& importSettings) {
		UsdStageRefPtr stage = UsdStage::Open(filePath);
		if (!stage) {
			spdlog::error("USD payload stage open failed for {}", filePath);
			return std::nullopt;
		}

		return LoadImportedAssetFromStage(stage, options, importSettings);
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
		const ImportSettings& importSettings) {
		const std::string identifierHint = options.layerIdentifierHint.empty() ? std::string("in_memory.usda") : options.layerIdentifierHint;
		SdfLayerRefPtr rootLayer = SdfLayer::CreateAnonymous(identifierHint);
		if (!rootLayer || !rootLayer->ImportFromString(usdText)) {
			spdlog::error("Failed to import in-memory USD payload layer '{}'.", identifierHint);
			return std::nullopt;
		}

		UsdStageRefPtr stage = UsdStage::Open(rootLayer);
		if (!stage) {
			spdlog::error("Failed to open in-memory USD payload stage '{}'.", identifierHint);
			return std::nullopt;
		}

		return LoadImportedAssetFromStage(stage, options, importSettings);
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
