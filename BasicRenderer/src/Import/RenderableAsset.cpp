#include "Import/RenderableAsset.h"

#include <unordered_set>

#include <BasicScene/Scene.h>

#include "Mesh/Mesh.h"
#include "Mesh/MeshInstance.h"
#include "Mesh/VertexFlags.h"
#include "Scene/Components.h"

namespace br::import {
namespace {

DirectX::XMMATRIX ComposeEntityLocalMatrix(flecs::entity entity)
{
	const auto* position = entity.try_get<Components::Position>();
	const auto* rotation = entity.try_get<Components::Rotation>();
	const auto* scale = entity.try_get<Components::Scale>();

	const auto scaleMatrix = scale ? DirectX::XMMatrixScalingFromVector(scale->scale) : DirectX::XMMatrixIdentity();
	const auto rotationMatrix = rotation ? DirectX::XMMatrixRotationQuaternion(rotation->rot) : DirectX::XMMatrixIdentity();
	const auto translationMatrix = position ? DirectX::XMMatrixTranslationFromVector(position->pos) : DirectX::XMMatrixIdentity();
	return scaleMatrix * rotationMatrix * translationMatrix;
}

void CollectRenderablePartsRecursive(
	flecs::entity entity,
	DirectX::XMMATRIX parentMatrix,
	RenderableAsset& asset,
	std::unordered_set<std::uint64_t>& meshIDs,
	std::uint32_t& skinnedShapeIndex)
{
	if (!entity || !entity.is_alive()) {
		return;
	}

	const auto localMatrix = ComposeEntityLocalMatrix(entity);
	const auto worldMatrix = localMatrix * parentMatrix;

	if (const auto* meshInstances = entity.try_get<Components::MeshInstances>()) {
		RenderableAssetPart part;
		part.localMatrix = worldMatrix;
		if (const auto* name = entity.try_get<Components::Name>()) {
			part.name = name->name;
		}

		bool hasSkinnedMesh = false;
		for (const auto& instance : meshInstances->meshInstances) {
			if (!instance) {
				continue;
			}

			auto mesh = instance->GetMesh();
			if (!IsRenderableMesh(mesh)) {
				continue;
			}

			part.meshes.push_back(mesh);
			hasSkinnedMesh = hasSkinnedMesh || IsSkinnedMesh(mesh);
			if (meshIDs.insert(mesh->GetGlobalID()).second) {
				asset.meshes.push_back(mesh);
			}
		}

		if (!part.meshes.empty()) {
			if (hasSkinnedMesh) {
				part.skinnedShapeIndex = skinnedShapeIndex++;
			}
			asset.parts.push_back(std::move(part));
		}
	}

	entity.children([&](flecs::entity child) {
		if (child.has<Components::SceneRoot>()) {
			return;
		}

		CollectRenderablePartsRecursive(child, worldMatrix, asset, meshIDs, skinnedShapeIndex);
	});
}

} // namespace

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

RenderableAsset RenderableAssetFromPayload(USDLoader::ImportedAssetPayload&& payload)
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

RenderableAsset CollectRenderableAssetFromScene(std::shared_ptr<Scene> scene)
{
	RenderableAsset asset;
	asset.scene = std::move(scene);
	if (asset.scene) {
		auto collected = CollectRenderableAssetFromScene(*asset.scene);
		collected.scene = std::move(asset.scene);
		return collected;
	}
	return asset;
}

RenderableAsset CollectRenderableAssetFromScene(Scene& scene)
{
	RenderableAsset asset;
	std::unordered_set<std::uint64_t> meshIDs;
	std::uint32_t skinnedShapeIndex = 0;
	auto root = scene.GetRoot();
	root.children([&](flecs::entity child) {
		if (child.has<Components::SceneRoot>()) {
			return;
		}

		CollectRenderablePartsRecursive(child, DirectX::XMMatrixIdentity(), asset, meshIDs, skinnedShapeIndex);
	});
	return asset;
}

} // namespace br::import
