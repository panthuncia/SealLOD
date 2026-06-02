#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "Scene/Components.h"

class Mesh;

namespace br::mesh {

Components::MeshInstances CreateMeshInstances(
	const std::vector<std::shared_ptr<Mesh>>& meshes,
	std::uint64_t generation = 1);

} // namespace br::mesh
