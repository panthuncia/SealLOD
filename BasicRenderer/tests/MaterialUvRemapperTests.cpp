#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Import/MaterialUvRemapper.h"
#include "Mesh/VertexFlags.h"
#include "Mesh/VertexLayout.h"

namespace {

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;
using DirectX::XMFLOAT4;

void Require(bool condition, const std::string& message)
{
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void RequireNear(float a, float b, const std::string& message)
{
	Require(std::abs(a - b) <= 1.0e-4f, message + " (" + std::to_string(a) + " vs " + std::to_string(b) + ")");
}

void WriteVertex(
	std::vector<std::byte>& vertices,
	std::uint32_t index,
	unsigned int stride,
	unsigned int flags,
	const XMFLOAT3& position,
	const XMFLOAT2& uv)
{
	const std::size_t base = static_cast<std::size_t>(index) * stride;
	const XMFLOAT3 normal{ 0.0f, 0.0f, 1.0f };
	const XMFLOAT4 tangent{ 1.0f, 0.0f, 0.0f, 1.0f };
	std::memcpy(vertices.data() + base + MeshVertexLayout::PositionOffset, &position, sizeof(position));
	std::memcpy(vertices.data() + base + MeshVertexLayout::NormalOffset, &normal, sizeof(normal));
	std::memcpy(vertices.data() + base + MeshVertexLayout::TangentOffset(flags), &tangent, sizeof(tangent));
	std::memcpy(vertices.data() + base + MeshVertexLayout::TexcoordOffset(flags), &uv, sizeof(uv));
}

void DuplicateIslandVerticesWeldToSharedGeneratedUv()
{
	const unsigned int flags = VERTEX_NORMALS | VERTEX_TANGENTS | VERTEX_TEXCOORDS;
	const unsigned int stride = MeshVertexLayout::VertexSize(flags);
	std::vector<std::byte> vertices(static_cast<std::size_t>(6u) * stride);
	std::vector<XMFLOAT2> uvValues{
		{ 0.0f, 0.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 1.0f },
		{ 11.0f, 10.0f },
		{ 11.0f, 11.0f },
		{ 10.0f, 11.0f },
	};

	WriteVertex(vertices, 0, stride, flags, XMFLOAT3{ 0.0f, 0.0f, 0.0f }, uvValues[0]);
	WriteVertex(vertices, 1, stride, flags, XMFLOAT3{ 1.0f, 0.0f, 0.0f }, uvValues[1]);
	WriteVertex(vertices, 2, stride, flags, XMFLOAT3{ 0.0f, 1.0f, 0.0f }, uvValues[2]);
	WriteVertex(vertices, 3, stride, flags, XMFLOAT3{ 1.0f, 0.0f, 0.0f }, uvValues[3]);
	WriteVertex(vertices, 4, stride, flags, XMFLOAT3{ 1.0f, 1.0f, 0.0f }, uvValues[4]);
	WriteVertex(vertices, 5, stride, flags, XMFLOAT3{ 0.0f, 1.0f, 0.0f }, uvValues[5]);

	std::vector<MeshUvSetData> uvSets{
		MeshUvSetData{
			.name = "st",
			.values = uvValues,
		},
	};
	const std::vector<std::uint32_t> indices{ 0, 1, 2, 3, 4, 5 };

	const auto result = br::import::RemapMaterialUvSet(
		vertices,
		stride,
		flags,
		indices,
		uvSets,
		br::import::MaterialUvRemapRequest{ .uvSetIndex = 0, .contextName = "duplicate island square" });

	Require(result.attempted, "remap was not attempted");
	Require(result.succeeded, "remap failed: " + result.message);
	Require(result.weldedVertexCount == 4u, "expected four welded vertices");
	Require(result.componentCount == 1u, "expected one connected component");
	Require(result.flippedTriangleCount == 0u, "expected no flipped triangles");
	RequireNear(uvSets[0].values[1].x, uvSets[0].values[3].x, "duplicate edge U did not weld");
	RequireNear(uvSets[0].values[1].y, uvSets[0].values[3].y, "duplicate edge V did not weld");
	RequireNear(uvSets[0].values[2].x, uvSets[0].values[5].x, "duplicate edge U did not weld");
	RequireNear(uvSets[0].values[2].y, uvSets[0].values[5].y, "duplicate edge V did not weld");
}

void MissingUvSetFailsCleanly()
{
	const unsigned int flags = VERTEX_NORMALS | VERTEX_TANGENTS | VERTEX_TEXCOORDS;
	const unsigned int stride = MeshVertexLayout::VertexSize(flags);
	std::vector<std::byte> vertices(static_cast<std::size_t>(3u) * stride);
	WriteVertex(vertices, 0, stride, flags, XMFLOAT3{ 0.0f, 0.0f, 0.0f }, XMFLOAT2{ 0.0f, 0.0f });
	WriteVertex(vertices, 1, stride, flags, XMFLOAT3{ 1.0f, 0.0f, 0.0f }, XMFLOAT2{ 1.0f, 0.0f });
	WriteVertex(vertices, 2, stride, flags, XMFLOAT3{ 0.0f, 1.0f, 0.0f }, XMFLOAT2{ 0.0f, 1.0f });

	std::vector<MeshUvSetData> uvSets;
	const std::vector<std::uint32_t> indices{ 0, 1, 2 };
	const auto result = br::import::RemapMaterialUvSet(
		vertices,
		stride,
		flags,
		indices,
		uvSets,
		br::import::MaterialUvRemapRequest{ .uvSetIndex = 0, .contextName = "missing uv set" });

	Require(result.attempted, "missing UV remap was not attempted");
	Require(!result.succeeded, "missing UV remap unexpectedly succeeded");
	Require(result.message.find("missing") != std::string::npos, "missing UV failure message was not useful");
}

} // namespace

int main()
{
	try {
		DuplicateIslandVerticesWeldToSharedGeneratedUv();
		MissingUvSetFailsCleanly();
		return 0;
	}
	catch (const std::exception& e) {
		std::cerr << "MaterialUvRemapperTests failed: " << e.what() << "\n";
		return 1;
	}
}
