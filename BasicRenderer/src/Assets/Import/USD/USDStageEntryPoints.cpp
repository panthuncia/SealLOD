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

namespace USDLoader {
	using namespace pxr;
	std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point begin);

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
