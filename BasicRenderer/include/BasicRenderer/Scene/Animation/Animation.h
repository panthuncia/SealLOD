#pragma once

#include <memory>
#include <unordered_map>
#include <string>

#include "BasicRenderer/Scene/Animation/AnimationClip.h"

class Animation {
public:
	Animation(std::string name) : name(name) {
	}
	std::string name;
	std::unordered_map<std::string, std::shared_ptr<AnimationClip>> nodesMap;

};
