#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <DirectXMath.h>
#include <BasicRenderer/Assets/Import/ObjectReyesAtlasCache.h>
#include <BasicRenderer/Assets/Import/RenderablePrototypeGeometry.h>

class Mesh;

namespace br::import {

struct RenderablePartPayload {
	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<br::import::RenderablePrototypeGeometry> prototypeGeometries;
	DirectX::XMMATRIX localMatrix{ DirectX::XMMatrixIdentity() };
	std::string name;
	std::uint32_t skinnedShapeIndex{ static_cast<std::uint32_t>(-1) };
};

struct ImportedAssetPayload {
	std::vector<std::shared_ptr<Mesh>> meshes;
	std::vector<std::uint64_t> meshMaterialHashes;
	std::vector<RenderablePartPayload> parts;
	std::optional<br::import::ObjectReyesAtlasCacheIdentity> objectReyesAtlasCacheIdentity;
	std::vector<std::string> cacheTextureSearchRoots;
};

} // namespace br::import
