#pragma once

#include <memory>
#include <optional>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include <pxr/usd/usd/stage.h>

#include <BasicRenderer/Assets/ImportSettings.h>
#include <BasicRenderer/Assets/Import/RenderablePrototypeGeometry.h>
#include <BasicRenderer/Assets/ImportedAsset.h>
#include <BasicRenderer/Assets/Import/ObjectReyesAtlasCache.h>
#include "BasicRenderer/Assets/MaterialDescription.h"

class Scene;
class Mesh;

namespace USDLoader {
	struct ObjectReyesBakedHeightMaterialEntry {
		std::string nifPath;
		std::vector<std::string> materialTexturePaths;
	};

	using ImportSettings = br::import::ImportSettings;

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
		std::string objectReyesHeightAtlasStorage = "r8_unorm";
		std::vector<ObjectReyesBakedHeightMaterialEntry> objectReyesBakedHeightMaterials;
		bool isUsdPackage = false;
		bool requireCachedAssembly = false;
		// Offline preprocessing requires a durable material-bucket assembly. When
		// enabled, assembly discovery/build failures must not fall through to the
		// ordinary expanded payload, because that would report a cache build as
		// successful without publishing anything the runtime can reopen.
		bool requireWholeAssetAssembly = false;
	};

	using RenderablePartPayload = br::import::RenderablePartPayload;
	using ImportedAssetPayload = br::import::ImportedAssetPayload;

	struct ImportTimingStats {
		std::uint64_t layerImportMs = 0;
		std::uint64_t stageOpenMs = 0;
		std::uint64_t meshPreprocessMs = 0;
		std::uint64_t payloadParseMs = 0;
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
		const ImportSettings& settings = {},
		ImportTimingStats* timingStats = nullptr);
	std::optional<ImportedAssetPayload> LoadImportedAssetFromStage(
		const pxr::UsdStageRefPtr& stage,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {},
		ImportTimingStats* timingStats = nullptr);
	std::optional<ImportedAssetPayload> LoadImportedAssetFromUsdBytes(
		const std::string& usdText,
		const InMemoryStageOptions& options,
		const ImportSettings& settings = {},
		ImportTimingStats* timingStats = nullptr);
}
