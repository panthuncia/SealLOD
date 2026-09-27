#include "BasicRenderer/Assets/Import/ModelLoader.h"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <spdlog/spdlog.h>

#include <BasicRenderer/Assets/Import/Filetypes.h>
#include "Assets/Import/Assimp/AssimpLoader.h"
#include "Assets/Import/GlTF/GlTFLoader.h"
#include "BasicRenderer/Assets/USD/USDLoader.h"
#include "BasicRenderer/Assets/Import/NifLoader.h"

std::shared_ptr<Scene> LoadModel(std::string filePath) {

	// Check if the file exists
	if (!std::filesystem::is_regular_file(filePath)) {
		spdlog::error("Model file not found: {}", filePath);
		return nullptr;
	}

	std::shared_ptr<Scene> scene;

	// Select loader based on file extension
	std::string extension = std::filesystem::path(filePath).extension().string();
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	SceneLoader loader = GetSceneLoader(extension);
	
	switch(loader) {
		case SceneLoader::GlTF:
			scene = GlTFLoader::LoadModel(filePath);
			break;
		case SceneLoader::Assimp:
			scene = AssimpLoader::LoadModel(filePath);
			break;
		case SceneLoader::OpenUSD:
			scene = USDLoader::LoadModel(filePath);
			break;
		case SceneLoader::Nif:
			scene = NifLoader::LoadModel(filePath);
			break;
	}

	return scene;
}
