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
#include <BasicRenderer/Assets/Import/USDMaterialCache.h>
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.h"
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

#include "Assets/Import/USD/USDImportState.h"
#include "Assets/Import/USD/USDMaterialConversion.h"

namespace USDLoader {
    using namespace pxr;
    using json = nlohmann::json;

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

}
