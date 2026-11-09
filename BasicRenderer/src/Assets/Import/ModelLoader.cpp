#include "BasicRenderer/Assets/Import/ModelLoader.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/Filetypes.h>
#include <BasicRenderer/Assets/Import/SceneManifest.h>
#include <memory>
#include "Assets/Import/GlTF/GlTFLoader.h"
#include "BasicRenderer/Assets/USD/USDLoader.h"
#include "BasicRenderer/Assets/Import/NifLoader.h"

std::optional<br::import::ImportedAssetPayload> br::import::LoadImportedAsset(
	const std::string& filePath, const ImportSettings& settings)
{
	std::error_code directoryError;
	if (std::filesystem::is_directory(filePath, directoryError)) {
		// A folder of scene caches: the one it holds.
		const auto manifests = FindSceneManifests(filePath);
		if (manifests.size() == 1) return LoadImportedAsset(manifests.front().string(), settings);
		spdlog::error("Model directory {} holds {} scene caches (*.brscene.json); name one", filePath, manifests.size());
		return std::nullopt;
	}
	if (!std::filesystem::is_regular_file(filePath)) {
		spdlog::error("Model file not found: {}", filePath);
		return std::nullopt;
	}
	const std::filesystem::path path(filePath);
	if (IsSceneManifestPath(path)) {
		// A scene cache: its root and profile, then the scene from the cache alone when the manifest describes it and its
		// CLod data is all there, else its source scene as usual.
		try {
			const SceneManifest manifest = ReadSceneManifest(path);
			(void)ApplySceneManifestCacheRoot(path);
			ImportSettings sceneSettings = settings;
			if (manifest.profile) sceneSettings.profile = std::make_shared<const ImportProfile>(*manifest.profile);
			sceneSettings.expectedSourceIdentifier = manifest.sourceIdentifier;
			if (manifest.scene && manifest.scene->notCacheableReason.empty()) {
				const auto location = SceneManifestSourceLocation(path, manifest);
				spdlog::info("Scene manifest {}: loading from the cache ({} parts, {} primitives)", filePath,
					manifest.scene->parts.size(), manifest.scene->primitives.size());
				// Primitives without CLod data are built from the source when it is there, and left out when it is not.
				std::error_code sourceError;
				const bool sourcePresent = std::filesystem::is_regular_file(location, sourceError);
				if (auto payload = GlTFLoader::LoadImportedAssetFromSceneCache(*manifest.scene, location.string(), sceneSettings, sourcePresent)) {
					return payload;
				}
				spdlog::warn("Scene manifest {}: the cache cannot load the scene alone; importing its source", filePath);
			}
			else if (manifest.scene) {
				spdlog::info("Scene manifest {}: not loadable from the cache alone ({}); importing its source", filePath,
					manifest.scene->notCacheableReason);
			}
			const auto source = ResolveSceneManifestSource(path, manifest);
			spdlog::info("Scene manifest {}: loading {}", filePath, source.string());
			return LoadImportedAsset(source.string(), sceneSettings);
		}
		catch (const std::exception& error) {
			spdlog::error("Scene manifest {}: {}", filePath, error.what());
			return std::nullopt;
		}
	}
	std::string extension = path.extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	switch (GetSceneLoader(extension)) {
	case SceneLoader::GlTF:
		return GlTFLoader::LoadImportedAssetFromFile(filePath, settings);
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
