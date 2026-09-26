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
#include <BasicRenderer/Assets/Import/USDMaterialCache.h>
#include <BasicRenderer/Assets/Import/CLodCacheLoader.h>
#include "Assets/GeometryProcessing/Reyes/ObjectReyesAtlasBaker.h"
#include <BasicRenderer/Assets/Import/USDGeometryExtractor.h>
#include <BasicRenderer/Assets/DefaultCLodSettings.h>
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"
#include "BasicRenderer/Assets/Geometry/VertexLayout.h"

#include "Assets/Import/USD/USDGeometryConversion.h"
#include "Assets/Import/USD/USDStageHelpers.h"

namespace USDLoader {
	using namespace pxr;
	using json = nlohmann::json;

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
		bool forceDoubleSided,
		const UsdPrim& meshPrim,
		const MeshPreprocessResult* preprocessResult,
		std::string_view staticTextureOverrideSourceName) {
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
		const std::string& sourceIdentifierOverride,
		bool retainArtifactsForAssetAssembly)
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

}
