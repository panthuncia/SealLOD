#pragma once

#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include <pxr/usd/usd/stage.h>

#include "Import/RenderablePrototypeGeometry.h"
#include "Materials/MaterialDescription.h"

class Scene;
class Mesh;

namespace USDLoader {
	struct ObjectReyesBakedHeightMaterialEntry {
		std::string nifPath;
		std::vector<std::string> materialTexturePaths;
	};

	struct ImportSettings {
		bool enableDoubleSidedNameHeuristic = true;
		bool loadMaterialTextures = true;
		std::uint32_t nifTessellationFactor = 1;
		std::vector<std::string> additionalTextureSearchRoots;
	};

	struct InMemoryStageOptions {
		std::string sourceIdentifier;
		std::string sourceDirectory;
		std::vector<std::string> textureSearchRoots;
		std::string layerIdentifierHint = "in_memory.usda";
		std::string objectReyesNifPath;
		std::string objectReyesConfigHash;
		std::vector<std::string> objectReyesTexturePaths;
		std::vector<std::string> objectReyesSurfaceSamplingTexturePaths;
		std::vector<std::string> objectReyesTriplanarProjectionTexturePaths;
		std::vector<std::string> objectReyesTripleTapStochasticTexturePaths;
		std::unordered_map<std::string, float> objectReyesDisplacementScaleOverrides;
		bool objectReyesNifMatched = false;
		bool objectReyesSurfaceSamplingEnabled = false;
		ObjectSurfaceSamplingMode objectReyesSurfaceSamplingMode = ObjectSurfaceSamplingMode::None;
		bool objectReyesSurfaceSamplingIncludeSelected = false;
		bool objectReyesSurfaceSamplingNifMatched = false;
		bool objectReyesTriplanarProjectionIncludeSelected = false;
		bool objectReyesTriplanarProjectionNifMatched = false;
		bool objectReyesTripleTapStochasticIncludeSelected = false;
		bool objectReyesTripleTapStochasticNifMatched = false;
		float objectReyesBoundaryBlendStripWidthObjectUnits = 8.0f;
		std::uint32_t objectReyesAtlasBakeResolution = 4096;
		std::uint32_t objectReyesAtlasBakePaddingTexels = 8;
		std::string objectReyesHeightAtlasStorage = "bc4u";
		std::vector<ObjectReyesBakedHeightMaterialEntry> objectReyesBakedHeightMaterials;
		bool isUsdPackage = false;
	};

	struct RenderablePartPayload {
		std::vector<std::shared_ptr<Mesh>> meshes;
		std::vector<br::import::RenderablePrototypeGeometry> prototypeGeometries;
		DirectX::XMMATRIX localMatrix{ DirectX::XMMatrixIdentity() };
		std::string name;
		std::uint32_t skinnedShapeIndex{ static_cast<std::uint32_t>(-1) };
	};

	struct ImportedAssetPayload {
		std::vector<std::shared_ptr<Mesh>> meshes;
		std::vector<std::uint64_t> meshMaterialHashes;
		std::vector<RenderablePartPayload> parts;
	};

	std::shared_ptr<Scene> LoadModel(std::string file, const ImportSettings& settings);
	std::shared_ptr<Scene> LoadModel(std::string file);
	std::shared_ptr<Scene> LoadModelFromFile(
		const std::string& filePath,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});
	std::shared_ptr<Scene> LoadModelFromStage(
		const pxr::UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});
	std::shared_ptr<Scene> LoadModelFromUsdBytes(
		const std::string& usdText,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});

	std::optional<ImportedAssetPayload> LoadImportedAssetFromFile(
		const std::string& filePath,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});
	std::optional<ImportedAssetPayload> LoadImportedAssetFromStage(
		const pxr::UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});
	std::optional<ImportedAssetPayload> LoadImportedAssetFromUsdBytes(
		const std::string& usdText,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {});
}
