#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

#include <DirectXMath.h>

#include <BasicRenderer/Assets/ImportedAsset.h>
#include <BasicRenderer/Assets/Import/ObjectReyesAtlasCache.h>
#include <optional>
#include <BasicRenderer/Assets/Import/RenderablePrototypeGeometry.h>

class Mesh;
class MeshInstance;

namespace br::import {

struct RenderableAssetPart
{
	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<RenderablePrototypeGeometry> prototypeGeometries;
	DirectX::XMMATRIX localMatrix{ DirectX::XMMatrixIdentity() };
	std::string name;
	std::uint32_t skinnedShapeIndex{ static_cast<std::uint32_t>(-1) };
};

struct RenderableAsset
{
	using RenderablePart = RenderableAssetPart;

	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<std::uint64_t> meshMaterialHashes;
	std::vector<RenderableAssetPart> parts;
	std::optional<ObjectReyesAtlasCacheIdentity> objectReyesAtlasCacheIdentity;
};

bool IsRenderableMesh(const std::shared_ptr<Mesh>& mesh);
bool IsUsableMeshInstance(const std::shared_ptr<MeshInstance>& instance);
bool IsSkinnedMesh(const std::shared_ptr<Mesh>& mesh);

RenderableAsset RenderableAssetFromPayload(ImportedAssetPayload&& payload);

} // namespace br::import
