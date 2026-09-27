#pragma once

#include <memory>
#include <string>

class Scene;

namespace GlTFLoader {
	std::shared_ptr<Scene> LoadModel(std::string filePath);
}
