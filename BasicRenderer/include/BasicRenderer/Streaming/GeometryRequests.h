#pragma once

#include <cstdint>
#include <memory>
#include "BasicRenderer/Extensions/ShaderBuffers.h"

class Mesh;
class Material;

namespace br::render {

struct StaticMeshTemplateRequest {
	std::shared_ptr<Mesh> mesh;
	std::shared_ptr<Material> material;
};

struct StaticMeshTemplateRegistration {
	uint32_t meshTemplateIndex = 0;
	uint32_t clodOffsetIndex = 0;
	uint32_t skinnedAssemblyTypeSlot = 0xFFFFFFFFu;
	BoundingSphere skinnedAssemblyBounds{};
	float skinnedBoundsScale = 1.0f;
	bool valid = false;
	bool pendingResources = false;
};

} // namespace br::render
