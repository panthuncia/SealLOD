#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "Import/MeshData.h"

namespace br::import {

struct MaterialUvRemapRequest {
	std::uint32_t uvSetIndex = 0;
	std::string contextName;
};

struct MaterialUvRemapResult {
	bool attempted = false;
	bool succeeded = false;
	std::string message;
	std::uint32_t uvSetIndex = 0;
	std::uint32_t weldedVertexCount = 0;
	std::uint32_t componentCount = 0;
	std::uint32_t flippedTriangleCount = 0;
	std::uint32_t validTriangleCount = 0;
};

MaterialUvRemapResult RemapMaterialUvSet(
	std::vector<std::byte>& vertices,
	unsigned int vertexStrideBytes,
	unsigned int vertexFlags,
	const std::vector<std::uint32_t>& indices,
	std::vector<MeshUvSetData>& uvSets,
	const MaterialUvRemapRequest& request);

} // namespace br::import
