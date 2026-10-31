#include "BasicRenderer/Assets/Import/ModelLoader.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/Filetypes.h>
#include "Assets/Import/GlTF/GlTFLoader.h"
#include "BasicRenderer/Assets/USD/USDLoader.h"
#include "BasicRenderer/Assets/Import/NifLoader.h"

std::optional<br::import::ImportedAssetPayload> br::import::LoadImportedAsset(
	const std::string& filePath, const ImportSettings& settings)
{
	if (!std::filesystem::is_regular_file(filePath)) {
		spdlog::error("Model file not found: {}", filePath);
		return std::nullopt;
	}
	const std::filesystem::path path(filePath);
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	switch (GetSceneLoader(extension)) {
	case SceneLoader::GlTF:
		return GlTFLoader::LoadImportedAssetFromFile(filePath);
	case SceneLoader::OpenUSD: {
		USDLoader::InMemoryStageOptions options{};
		options.sourceIdentifier = filePath;
		options.sourceDirectory = path.parent_path().string();
		options.layerIdentifierHint = path.filename().string();
		options.isUsdPackage = extension == ".usdz";
		options.requireCachedAssembly = false;
		return USDLoader::LoadImportedAssetFromFile(filePath, options, settings);
	}
	case SceneLoader::Nif:
		return NifLoader::LoadImportedAssetWithCacheKey(filePath, filePath, settings);
	case SceneLoader::UNKNOWN:
		break;
	}
	spdlog::error("No payload loader for model '{}'", filePath);
	return std::nullopt;
}
