#include "BasicRenderer/Assets/Geometry/MeshInstanceFactory.h"

#include <BasicRenderer/Assets/RenderableAsset.h>
#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"

namespace br::mesh {

Components::MeshInstances CreateMeshInstances(
	const std::vector<std::shared_ptr<Mesh>>& meshes,
	std::uint64_t generation)
{
	Components::MeshInstances meshInstances;
	meshInstances.generation = generation;
	for (const auto& mesh : meshes) {
		if (!br::import::IsRenderableMesh(mesh)) {
			continue;
		}
		meshInstances.meshInstances.push_back(MeshInstance::CreateShared(mesh));
	}
	return meshInstances;
}

} // namespace br::mesh
