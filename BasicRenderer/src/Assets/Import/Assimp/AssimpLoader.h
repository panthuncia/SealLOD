#pragma once

#include <memory>
#include <string>

class Scene;

namespace AssimpLoader {
	std::shared_ptr<Scene> LoadModel(std::string file);
}