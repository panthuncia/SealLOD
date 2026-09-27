#pragma once

#include <memory>
#include <string>

class Scene;
std::shared_ptr<Scene> LoadModel(std::string file);