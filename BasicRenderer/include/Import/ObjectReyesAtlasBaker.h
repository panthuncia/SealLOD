#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "Import/MeshData.h"

namespace br::import {

struct ObjectReyesAtlasSourceMesh {
	const std::vector<std::byte>* vertices = nullptr;
	const std::vector<std::uint32_t>* indices = nullptr;
	const std::vector<MeshUvSetData>* uvSets = nullptr;
	std::uint32_t vertexSize = 0;
	std::uint32_t vertexFlags = 0;
	std::uint32_t materialIndex = 0;
};

struct ObjectReyesAtlasBakeOptions {
	float texelsPerUnit = 1.0f;
	std::uint32_t maxAtlasSize = 2048;
	std::uint32_t resolution = 0;
	std::uint32_t paddingTexels = 8;
};

struct ObjectReyesAtlasBakeResult {
	bool success = false;
	std::string error;
	std::vector<std::byte> vertices;
	std::vector<std::uint32_t> indices;
	std::vector<MeshUvSetData> uvSets;
	std::vector<std::uint32_t> triangleMaterialIndices;
	std::uint32_t atlasUvSetIndex = 0;
	std::uint32_t atlasWidth = 0;
	std::uint32_t atlasHeight = 0;
	float texelsPerUnit = 1.0f;
};

ObjectReyesAtlasBakeResult BuildObjectReyesAtlasBakedHeightMesh(
	std::span<const ObjectReyesAtlasSourceMesh> meshes,
	const ObjectReyesAtlasBakeOptions& options,
	std::string_view debugName);

} // namespace br::import
