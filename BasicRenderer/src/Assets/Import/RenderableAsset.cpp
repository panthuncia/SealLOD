#include <BasicRenderer/Assets/RenderableAsset.h>

#include "BasicRenderer/Assets/Geometry/Mesh.h"
#include "BasicRenderer/Assets/Geometry/MeshInstance.h"
#include <BasicRenderer/Scene/VertexFlags.h>

namespace br::import {

bool IsRenderableMesh(const std::shared_ptr<Mesh>& mesh)
{
	return mesh && mesh->material != nullptr;
}

bool IsUsableMeshInstance(const std::shared_ptr<MeshInstance>& instance)
{
	return instance && IsRenderableMesh(instance->GetMesh());
}

bool IsSkinnedMesh(const std::shared_ptr<Mesh>& mesh)
{
	return mesh && (mesh->GetPerMeshCBData().vertexFlags & VertexFlags::VERTEX_SKINNED) != 0u;
}

RenderableAsset RenderableAssetFromPayload(ImportedAssetPayload&& payload)
{
	RenderableAsset asset;
	asset.meshes = std::move(payload.meshes);
	asset.meshMaterialHashes = std::move(payload.meshMaterialHashes);
	asset.objectReyesAtlasCacheIdentity = std::move(payload.objectReyesAtlasCacheIdentity);
	asset.parts.reserve(payload.parts.size());
	for (auto& payloadPart : payload.parts) {
		RenderableAssetPart part;
		part.meshes = std::move(payloadPart.meshes);
		part.prototypeGeometries = std::move(payloadPart.prototypeGeometries);
		part.localMatrix = payloadPart.localMatrix;
		part.name = std::move(payloadPart.name);
		part.skinnedShapeIndex = payloadPart.skinnedShapeIndex;
		asset.parts.push_back(std::move(part));
	}
	return asset;
}

} // namespace br::import
