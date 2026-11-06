#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "BasicRenderer/Extensions/ShaderBuffers.h"

class Mesh;
class Material;

namespace br::render {

struct StaticMeshTemplateRequest {
	std::shared_ptr<Mesh> mesh;
	std::shared_ptr<Material> material;
	// A hierarchy root's palette rows (Mesh::GetMaterialPalette order), each built from a material its template
	// admitted. Empty for an ordinary mesh.
	std::vector<CLodMaterialPaletteEntry> palette;
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
